#!/usr/bin/env python3
"""Helpers for `local_ci.sh --premerge` (the M7 gate).

  premerge.py overlap [--since REF]   does dev_claude's movement touch files the PR also touches?
  premerge.py tests                   the PR's own tests: a ctest -R regex, and the targets to build

The PR's own files are the ones changed since the merge base with origin/dev_claude (MCE_TIDY_BASE), the
same set local_ci.sh uses. The base of the last green `--task` run is recorded in
build/local-ci-logs/last-task.env (BASE=<merge base sha>); `--since` overrides it. Without either,
overlap cannot be decided and the script says a --task run is needed.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE_REF = os.environ.get("MCE_TIDY_BASE", "origin/dev_claude")
RECORD = ROOT / "build" / "local-ci-logs" / "last-task.env"

# Always part of the gate: the MCP transcript golden (drives the real mce_mcp, see tests/mcp).
ALWAYS_TESTS = ["mcp_transcript"]
ALWAYS_TARGETS = ["mce_mcp"]

ADD_TEST = re.compile(r"\badd_test\s*\(\s*NAME\s+([A-Za-z0-9_.+-]+)")
MCE_ADD_TEST = re.compile(r"\bmce_add_test\s*\(\s*([A-Za-z0-9_.+-]+)")
ADD_TEST_BLOCK = re.compile(r"\badd_test\s*\(([^)]*(?:\$<[^>]*>[^)]*)*)\)", re.S)
TARGET_FILE = re.compile(r"\$<TARGET_FILE:([A-Za-z0-9_.+-]+)>")
ADD_TARGET = re.compile(r"\badd_(?:executable|library)\s*\(\s*([A-Za-z0-9_.+-]+)")


def git(*args):
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def merge_base():
    out = git("merge-base", "HEAD", BASE_REF)
    return out.strip() if out else None


def pr_files():
    """Files the PR changes relative to dev_claude, committed or not."""
    mb = merge_base()
    if not mb:
        return None
    files = set((git("diff", "--name-only", mb) or "").split())
    files |= set((git("ls-files", "--others", "--exclude-standard") or "").split())
    return files


def recorded_base():
    if RECORD.is_file():
        for line in RECORD.read_text().splitlines():
            if line.startswith("BASE="):
                return line[5:].strip()
    return None


def module_of(path):
    """The directory a file's module lives in: src/engine/<mod>, tests/<mod>, samples/<x>, projects/<x>."""
    parts = path.split("/")
    if parts[0] == "src" and len(parts) > 2 and parts[1] in ("engine", "editor"):
        return "/".join(parts[:3])
    if parts[0] in ("src", "tests", "samples", "projects", "templates", "tools") and len(parts) > 2:
        return "/".join(parts[:2])
    return None


def cmd_overlap(args):
    files = pr_files()
    if files is None:
        print(f"premerge: no merge base with {BASE_REF}; run --task")
        return 3
    since = args.since or recorded_base()
    if not since or git("cat-file", "-e", since + "^{commit}") is None:
        print("premerge: no recorded --task base (build/local-ci-logs/last-task.env) and no --since; run --task")
        return 3
    head_base = BASE_REF
    moved = set((git("diff", "--name-only", f"{since}..{head_base}") or "").split())
    # Documents (*.md) never need a --task re-run: nothing built, tested or read by scripts changes with them.
    code_files = {f for f in files if not f.endswith(".md")}
    moved = {f for f in moved if not f.endswith(".md")}
    both = sorted(code_files & moved)
    mods = {module_of(f) for f in code_files} - {None}
    near = sorted({f for f in moved - code_files if module_of(f) in mods})
    print(f"premerge: PR changes {len(files)} file(s); {BASE_REF} moved {len(moved)} file(s) since the --task base {since[:10]}")
    if both:
        print("premerge: OVERLAP, both sides changed:")
        for f in both:
            print("  " + f)
        print("premerge: a --task re-run IS needed")
        return 1
    if near:
        print(f"premerge: dev_claude changed {len(near)} other file(s) in modules the PR touches:")
        for f in near[:20]:
            print("  " + f)
        print("premerge: same-module change, a --task re-run IS needed")
        return 1
    print("premerge: no file overlap and no change in the PR's modules, a --task re-run is NOT needed")
    print("premerge: a semantic clash elsewhere (a header the PR includes) is caught by the integration run")
    return 0


def nearest_cmakelists(path):
    d = (ROOT / path).parent
    while d != ROOT.parent:
        c = d / "CMakeLists.txt"
        if c.is_file():
            return c
        if d == ROOT:
            break
        d = d.parent
    return None


def snake(name):
    return re.sub(r"(?<=[a-z])(?=[A-Z])", "_", name).lower()


def test_targets(text, known):
    """Plain add_test(NAME x COMMAND ...) -> {test: target or None}: the COMMAND word when it is a target of this
    CMakeLists or $<TARGET_FILE:t>, else the first $<TARGET_FILE:t> in the statement."""
    out = {}
    for body in ADD_TEST_BLOCK.findall(text):
        words = body.split()
        if len(words) < 3 or words[0] != "NAME":
            continue
        name, target = words[1], None
        if "COMMAND" in words:
            cmd = words[words.index("COMMAND") + 1:][:1]
            m = TARGET_FILE.fullmatch(cmd[0]) if cmd else None
            if m:
                target = m.group(1)
            elif cmd and cmd[0] in known:
                target = cmd[0]
        if target is None:
            m = TARGET_FILE.search(body)
            target = m.group(1) if m else None
        out[name] = target
    return out


def cmd_tests(_args):
    files = pr_files() or set()
    tests = {}  # name -> True (mce_add_test: the target has the test's name), a target name, or False (unknown)
    prefixes = set()
    for f in sorted(files):
        if not (ROOT / f).is_file():
            continue
        # Only the source trees that define tests: a docs-only PR selects nothing.
        if not re.match(r"(src|samples|tests|projects|tools|templates)/", f):
            continue
        cm = nearest_cmakelists(f)
        if cm is None:
            continue
        text = "\n".join(line.split("#", 1)[0] for line in cm.read_text(errors="replace").splitlines())
        for n in MCE_ADD_TEST.findall(text):
            tests[n] = True
        known = set(ADD_TARGET.findall(text)) | set(MCE_ADD_TEST.findall(text))
        for n, target in test_targets(text, known).items():
            if n not in tests:
                tests[n] = target if target else False
        # mce_project() registers <game>_* tests from the directory name (cmake/MceProject.cmake)
        m = re.match(r"projects/([^/]+)/", f)
        if m:
            prefixes.add(snake(m.group(1)) + "_")
    names = sorted(set(tests) | set(ALWAYS_TESTS))
    regex = "^(" + "|".join(re.escape(n) for n in names) + ")$"
    if prefixes:
        regex = "(" + regex + "|^(" + "|".join(re.escape(p) for p in sorted(prefixes)) + "))"
    targets = {ALWAYS_TARGETS[0]}
    for n, t in tests.items():
        if t is True:
            targets.add(n)
        elif t:
            targets.add(t)
    targets = sorted(targets)
    # A test whose target we cannot name makes the shell build everything; a project's tests (prefix rule) need
    # the game and its content, which the full build covers.
    full_build = any(t is False for t in tests.values()) or bool(prefixes)
    print("REGEX=" + regex)
    print("TARGETS=" + " ".join(targets))
    print("FULL_BUILD=" + ("1" if full_build else "0"))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    o = sub.add_parser("overlap")
    o.add_argument("--since")
    sub.add_parser("tests")
    args = ap.parse_args()
    return {"overlap": cmd_overlap, "tests": cmd_tests}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
