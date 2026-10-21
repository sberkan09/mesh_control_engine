#!/usr/bin/env python3
"""How many translation units include a changed header (M3): the cost of a header change made visible.

    scripts/ci/include_fanout.py                  # headers changed since the merge base with origin/master
    scripts/ci/include_fanout.py --all            # the 25 widest headers of the tree
    scripts/ci/include_fanout.py path/to/a.h ...  # these headers
    options: --warn N (default 150)   --base REF (default $MCE_TIDY_BASE or origin/master)

A header that reaches N or more .cpp/.cc/.c/.mm files, directly or through other headers, makes every build
tree after the merge rebuild all of them: the ccache hit rate of every container drops. Above the threshold
it prints a WARNING and exits 0 (the walls stage shows it, it does not fail): ship such a change as its own
small seam PR, merged first, prefer changing a .cpp, forward-declare, and say in the PR description why the
header has to change.

Includes are resolved textually: `#include "a/b.h"` / `<a/b.h>` match a header whose path ends in a/b.h
(quoted includes also try the including file's directory first). A one-component include matches by
basename, so counts are upper bounds. Generated headers and system headers are
not in the tree and are ignored.
"""

import argparse
import collections
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCAN_DIRS = ["src", "samples", "projects", "templates", "tests", "tools"]
HEADER_EXT = {".h", ".hpp", ".inl"}
TU_EXT = {".c", ".cc", ".cpp", ".mm"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)


def scan():
    """(tus, headers, edges): edges maps a file to the repo files it includes."""
    files = []
    for d in SCAN_DIRS:
        base = ROOT / d
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [n for n in dirnames if n not in ("build", "third_party", ".git")]
            for n in filenames:
                p = pathlib.Path(dirpath, n)
                if p.suffix in HEADER_EXT | TU_EXT:
                    files.append(p.relative_to(ROOT).as_posix())
    by_tail = collections.defaultdict(list)  # last two components -> headers
    for f in files:
        if pathlib.PurePosixPath(f).suffix in HEADER_EXT:
            parts = f.split("/")
            by_tail["/".join(parts[-2:])].append(f)
            by_tail[parts[-1]].append(f)
    edges = {}
    for f in files:
        text = (ROOT / f).read_text(encoding="utf-8", errors="replace")
        out = set()
        for kind, inc in INCLUDE.findall(text):
            if kind == '"':
                local = (pathlib.PurePosixPath(f).parent / inc).as_posix()
                if (ROOT / local).is_file():
                    out.add(os.path.normpath(local).replace(os.sep, "/"))
                    continue
            parts = inc.split("/")
            for cand in by_tail.get("/".join(parts[-2:]), []) if len(parts) > 1 else by_tail.get(inc, []):
                if cand.endswith(inc):
                    out.add(cand)
        edges[f] = out
    tus = [f for f in files if pathlib.PurePosixPath(f).suffix in TU_EXT]
    return tus, edges


def fanout(tus, edges):
    """header -> number of TUs reaching it, through any chain of includes."""
    users = collections.defaultdict(set)  # file -> the files that include it directly
    for f, incs in edges.items():
        for i in incs:
            users[i].add(f)

    def reach(h):
        seen, todo = {h}, [h]
        while todo:
            for u in users.get(todo.pop(), ()):
                if u not in seen:
                    seen.add(u)
                    todo.append(u)
        return seen

    return reach


def changed_headers(base):
    mb = subprocess.run(["git", "merge-base", "HEAD", base], cwd=ROOT, capture_output=True, text=True)
    if mb.returncode != 0:
        return []
    names = subprocess.run(["git", "diff", "--name-only", mb.stdout.strip()], cwd=ROOT, capture_output=True,
                           text=True).stdout.split()
    names += subprocess.run(["git", "ls-files", "--others", "--exclude-standard"], cwd=ROOT, capture_output=True,
                            text=True).stdout.split()
    return sorted({n for n in names if pathlib.PurePosixPath(n).suffix in HEADER_EXT and (ROOT / n).is_file()})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("headers", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--warn", type=int, default=150)
    ap.add_argument("--base", default=os.environ.get("MCE_TIDY_BASE", "origin/master"))
    args = ap.parse_args()

    tus, edges = scan()
    tu_set = set(tus)
    reach = fanout(tus, edges)
    if args.all:
        headers = [f for f in edges if pathlib.PurePosixPath(f).suffix in HEADER_EXT]
        counts = sorted(((len(reach(h) & tu_set), h) for h in headers), reverse=True)[:25]
    else:
        headers = args.headers or changed_headers(args.base)
        counts = sorted(((len(reach(h) & tu_set), h) for h in headers), reverse=True)
    if not counts:
        print("include fanout: no changed header")
        return 0
    wide = 0
    for n, h in counts:
        mark = "  WARNING" if n >= args.warn else ""
        print(f"include fanout: at most {n:5d} of {len(tus)} TUs include {h}{mark}")
        wide += n >= args.warn
    if wide and not args.all:
        print(f"include fanout: WARNING, {wide} header(s) reach {args.warn}+ TUs: every build tree rebuilds them after the merge.")
        print("include fanout: ship the header change as its own small seam PR merged first (prefer a .cpp, forward-declare),")
        print("include fanout: and say in the PR description why the header has to change.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
