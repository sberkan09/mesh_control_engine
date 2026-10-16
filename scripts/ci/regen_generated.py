#!/usr/bin/env python3
"""Regenerate the generated files that go stale when PRs merge (M2): the integration runner's job.

    scripts/ci/regen_generated.py [--exe build/linux-gcc-debug/src/editor/mcp/mce_mcp]

  docs/engine/capabilities.md           scripts/ci/capabilities.py (rewrites the table)
  scripts/ci/sample_coverage.allow      drops the lines capabilities.py reports as stale: a component a sample
                                        now shows, or an entry that names no component
  tests/mcp/transcripts/*.golden.jsonl  tests/mcp/run_transcript.py --update (needs --exe, the built mce_mcp)

It never edits sources or docs: a gap that needs a human (a component no document names, a host that does not
register a component) is printed and makes the exit code 1. Prints the files it changed, one per line, after
`changed:`.
"""

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
ALLOW = ROOT / "scripts/ci/sample_coverage.allow"
STALE = [
    re.compile(r"component (\w+): .* now shows it; remove it from"),
    re.compile(r"sample_coverage\.allow: (\w+) is not a component"),
]


def run(*cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def capabilities():
    r = run(sys.executable, "scripts/ci/capabilities.py")
    stale = {m.group(1) for line in r.stdout.splitlines() for rx in STALE if (m := rx.search(line))}
    if stale:
        kept = [ln for ln in ALLOW.read_text(encoding="utf-8").splitlines(keepends=True)
                if ln.split("|", 1)[0].strip() not in stale]
        ALLOW.write_text("".join(kept), encoding="utf-8")
        print(f"sample_coverage.allow: dropped {', '.join(sorted(stale))}")
        r = run(sys.executable, "scripts/ci/capabilities.py")
    sys.stdout.write(r.stdout)
    return r.returncode


def golden(exe):
    r = run(sys.executable, "tests/mcp/run_transcript.py", "--exe", exe, "--update",
            "--work", str(ROOT / "build" / "regen-transcript"))
    sys.stdout.write(r.stdout + r.stderr)
    return r.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="built mce_mcp; without it the transcript golden is left alone")
    args = ap.parse_args()
    bad = capabilities()
    if args.exe:
        bad |= golden(args.exe)
    changed = run("git", "status", "--porcelain", "--", "docs/engine/capabilities.md",
                  "scripts/ci/sample_coverage.allow", "tests/mcp/transcripts").stdout.splitlines()
    for line in changed:
        print("changed: " + line[3:])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
