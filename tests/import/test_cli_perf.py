#!/usr/bin/env python3
"""M-CONTENT 2: `mce perf` reads a .mcperf capture and prints the draw calls by cause.

    test_cli_perf.py <work dir>

A hand-written capture stands in for the player's (no SDK install or build is needed); the counts are the
machine-independent part of a capture, so the table is compared exactly.
"""

import contextlib
import io
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "mce_cli"))
import mce  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def run(argv):
    out = io.StringIO()
    code = None
    with contextlib.redirect_stdout(out):
        try:
            code = mce.main(argv)
        except SystemExit as e:
            code = e.code
    return code, out.getvalue()


NAMES = ["material", "unique_mesh", "skinned", "terrain", "moving", "blended", "instancing_off", "budget", "other"]
HINTS = [f"hint for {n}" for n in NAMES]


def capture(frames):
    return {
        "format": "mce-perf", "version": 1, "app": "mce_player", "backend": "opengl", "scene": "demo",
        "draw_causes": NAMES, "draw_cause_hints": HINTS,
        "summary": {"frames": len(frames), "frame_ms": {"count": 2, "avg": 1, "p50": 1.0, "p95": 2.0, "p99": 2, "max": 2.0},
                    "counters_avg": {"draws": 30, "triangles": 1000, "meshes": 20, "sprites": 0}},
        "frames": [{"counters": {"draws": 30, "runs_instanced": 2, "draws_instanced": 20, "draw_causes": f}} for f in frames],
    }


def main(work):
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "game", "cooked", ".perf"))
    # Two frames: mean material 4 (max 5), unique_mesh 2, budget 1 (3 and 0: mean 1, max 3).
    frames = [[5, 2, 0, 0, 0, 0, 0, 3, 0], [3, 2, 0, 0, 0, 0, 0, 0, 0]]
    path = os.path.join(work, "game", "cooked", ".perf", "last.mcperf")
    with open(path, "w") as f:
        json.dump(capture(frames), f)

    code, text = run(["perf", os.path.join(work, "game")])
    check(code == 0, "mce perf <project> reads cooked/.perf/last.mcperf")
    check("draw calls by cause" in text, "the table has its heading")
    lines = [l.split() for l in text.splitlines()]
    row = {l[0]: l for l in lines if l and l[0] in NAMES + ["instanced", "total"]}
    check(row.get("material", [])[:3] == ["material", "4", "5"], "material: mean 4, max 5")
    check(row.get("unique_mesh", [])[:3] == ["unique_mesh", "2", "2"], "unique_mesh: 2")
    check(row.get("budget", [])[:3] == ["budget", "1", "3"], "budget: mean 1, max 3")
    check("skinned" not in row, "causes that never happen are not listed")
    check(row.get("instanced", [])[:2] == ["instanced", "20"], "instanced draws are listed first")
    check(row.get("total", [])[:2] == ["total", "27"], "total = instanced + causes")
    check("hint for material" in text and "hint for budget" in text, "each listed cause has its hint")
    check(text.index("  material") < text.index("  unique_mesh") < text.index("  budget"), "largest mean first")

    code, text = run(["perf", path, "--json"])
    doc = json.loads(text)
    check(code == 0 and [c["cause"] for c in doc["draw_causes"]] == ["material", "unique_mesh", "budget"], "--json lists the causes")
    check(doc["instanced_runs"] == 2 and doc["instanced_draws"] == 20, "--json has the instanced counts")

    # A capture from before the report: no names, no counts.
    old = capture([[0] * 9])
    del old["draw_causes"], old["draw_cause_hints"]
    for fr in old["frames"]:
        del fr["counters"]["draw_causes"]
    with open(path, "w") as f:
        json.dump(old, f)
    code, text = run(["perf", path])
    check(code == 0 and "draw calls by cause" in text, "an old capture still prints")

    code, text = run(["perf", os.path.join(work, "nothing.mcperf")])
    check(code not in (0, None) and "does not exist" in str(code), "a missing file exits with what to do")
    with open(path, "w") as f:
        f.write('{"format": "mce-scene"}')
    code, text = run(["perf", path])
    check(code not in (0, None) and "not a .mcperf" in str(code), "another JSON file is refused")

    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
