#!/usr/bin/env python3
"""Determinism harness (FR-RUN-040, ADR-0007): run a sample several times per binary and require
every run's final state hash to match — across runs and across binaries (Debug vs Release).

    determinism.py --runs 3 --args "-ticks 1000 -boids 20000" build/a/sample build/b/sample
"""

import argparse
import re
import shlex
import subprocess
import sys

HASH_RE = re.compile(r"hash ([0-9a-f]{16})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--args", default="")
    ap.add_argument("binaries", nargs="+")
    a = ap.parse_args()

    hashes = {}
    for binary in a.binaries:
        for i in range(a.runs):
            out = subprocess.run([binary, *shlex.split(a.args)], capture_output=True, text=True, check=False)
            m = HASH_RE.search(out.stdout)
            if out.returncode != 0 or not m:
                print(f"FAIL {binary} run {i}: rc={out.returncode}\n{out.stdout}\n{out.stderr}")
                return 1
            hashes[(binary, i)] = m.group(1)
            print(f"{binary} run {i}: {m.group(1)}")
    if len(set(hashes.values())) != 1:
        print("FAIL: state hashes differ")
        return 1
    print(f"OK: {len(hashes)} runs, hash {next(iter(hashes.values()))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
