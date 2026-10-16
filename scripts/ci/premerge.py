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
    both = sorted(files & moved)
    print(f"premerge: PR changes {len(files)} file(s); {BASE_REF} moved {len(moved)} file(s) since the --task base {since[:10]}")
    if both:
        print("premerge: OVERLAP, both sides changed:")
        for f in both:
            print("  " + f)
        print("premerge: a --task re-run IS needed")
        return 1
    print("premerge: no overlap, a --task re-run is NOT needed")
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


def cmd_tests(_args):
    files = pr_files() or set()
    tests = {}  # name -> True when it is a target built by mce_add_test
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
        for n in ADD_TEST.findall(text):
            tests.setdefault(n, False)
    names = sorted(set(tests) | set(ALWAYS_TESTS))
    regex = "^(" + "|".join(re.escape(n) for n in names) + ")$"
    targets = sorted({n for n, is_target in tests.items() if is_target} | set(ALWAYS_TARGETS))
    # A test defined with plain add_test may need targets we cannot name: the shell then builds everything.
    full_build = any(not t for t in tests.values())
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
