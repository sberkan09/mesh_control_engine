#!/usr/bin/env python3
"""The M-PERF counter gate: a project's render counters may not grow without a reviewed baseline.

    perf_gate.py --mcp <game>_mcp --project projects/<Name>             compare with perf/counters.json
    perf_gate.py --mcp <game>_mcp --project projects/<Name> --update    write the measured counters there
    perf_gate.py --mcp <game>_mcp --project projects/<Name> --seed      the gate must catch a planted regression

perf/counters.json lists the runs and the counters each one had: {"runs": [{"scene": "scenes/level1.mcscene",
"frames": 60, "warmup": 10, "width": 480, "height": 270, "counters": {"draws": 12, ...}}]}. Each run is MCP
perf_run on a fresh copy of the project (--work, default a temp folder), one sim tick per frame, so the
counters are the same on every machine. A counter above its baseline fails the gate; one below it passes
with a note to run --update, so the baseline follows the improvement. Times are printed, never judged.

--seed adds three sprites, each with its own texture, to the first run's scene in the copy, and passes only
when the gate then fails on draws: the check that the gate can fail at all.
Exit code 0 when the gate holds ("gate: pass").
"""

import argparse
import json
import pathlib
import shutil
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "agent_demo"))
from demo_gate import SKIP, Server  # noqa: E402

# Counters the gate holds: what the frame asks of the GPU. Upload bytes and heap allocations are
# printed but not held (they follow buffer growth policy and the allocator, not the scene).
HELD = ["draws", "passes", "dispatches", "triangles", "meshes", "sprites", "sprite_batches"]
RUN_KEYS = ["frames", "warmup", "width", "height"]


def png(w, h, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\0" + bytes(rgba) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def seed(s, project, scene):
    """Three sprites with a texture each, in `scene`: three more batches, so more draws."""
    ok, _, text = s.tool("scene_open", {"path": scene})
    if not ok:
        raise RuntimeError(f"scene_open {scene}: {text[:300]}")
    for i in range(3):
        guid = f"5eed0000-0000-4000-8000-00000000000{i}"
        tex = project / "assets" / "textures" / f"perf_seed{i}.png"
        tex.parent.mkdir(parents=True, exist_ok=True)
        tex.write_bytes(png(4, 4, (40 + 60 * i, 200, 90, 255)))
        tex.with_name(tex.name + ".mceasset").write_text(
            json.dumps({"guid": guid, "importer": "texture", "importer_version": 1, "settings": {}}) + "\n", encoding="utf-8")
        ok, _, text = s.tool("entity_create", {"name": f"PerfSeed{i}", "components": {
            "Sprite": {"texture": f"guid:{guid}"}, "LocalTransform": {"position": [i, 1, 0]}}})
        if not ok:
            raise RuntimeError(f"entity_create: {text[:300]}")
    ok, _, text = s.tool("scene_save", {})
    if not ok:
        raise RuntimeError(f"scene_save: {text[:300]}")


def measure(s, run):
    args = {"scene": run["scene"], "target": run.get("target", "desktop"), "top": 3}
    for k in RUN_KEYS:
        if k in run:
            args[k] = run[k]
    ok, out, text = s.tool("perf_run", args)
    if not ok or out is None:
        raise RuntimeError(f"perf_run {run['scene']}: {text[:400]}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcp", required=True)
    ap.add_argument("--project", required=True, type=pathlib.Path)
    ap.add_argument("--work", type=pathlib.Path, help="copy the project here (default: a temp folder)")
    ap.add_argument("--update", action="store_true", help="write the measured counters to perf/counters.json")
    ap.add_argument("--seed", action="store_true", help="plant a draw regression; pass only when the gate catches it")
    a = ap.parse_args()
    source = a.project.resolve()
    baseline_path = source / "perf" / "counters.json"
    if not baseline_path.is_file():
        print(f"FAIL {baseline_path} is missing (write it with --update after adding a run)")
        print("gate: FAIL")
        return 1
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    runs = baseline.get("runs", [])
    if not runs:
        print(f"FAIL {baseline_path} lists no runs")
        print("gate: FAIL")
        return 1

    tmp = None
    work = a.work
    if work is None:
        tmp = tempfile.TemporaryDirectory(prefix="perf_gate_")
        work = pathlib.Path(tmp.name) / source.name
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(source, work, ignore=lambda d, names: [n for n in names if n in SKIP])

    problems, notes = [], []
    s = Server(a.mcp, work.resolve())
    try:
        if a.seed:
            seed(s, work.resolve(), runs[0]["scene"])
        for run in runs:
            out = measure(s, run)
            got = out.get("counters_max", {})
            want = run.get("counters", {})
            frame = out.get("frame_ms", {})
            print(f"  {run['scene']}: " + ", ".join(f"{k} {got.get(k, 0)}" for k in HELD) +
                  f"; frame p95 {frame.get('p95', 'n/a')} ms (not judged)")
            for k in HELD:
                g, w = int(got.get(k, 0)), want.get(k)
                if w is None:
                    if not a.update:
                        notes.append(f"{run['scene']}: {k} {g} has no baseline")
                elif g > w:
                    problems.append(f"{run['scene']}: {k} {g} > baseline {w}")
                elif g < w:
                    notes.append(f"{run['scene']}: {k} {g} < baseline {w}: better; run --update to keep it")
            if a.update:
                run["counters"] = {k: int(got.get(k, 0)) for k in HELD}
    finally:
        s.close()
        if tmp:
            tmp.cleanup()

    for n in notes:
        print("note " + n)
    if a.update:
        baseline_path.write_text(json.dumps(baseline, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {baseline_path}")
        print("gate: pass")
        return 0
    if a.seed:
        caught = any(": draws " in p for p in problems)
        for p in problems:
            print("caught " + p)
        print("gate: " + ("pass (the planted regression failed the gate)" if caught else "FAIL (the planted regression went through)"))
        return 0 if caught else 1
    for p in problems:
        print("FAIL " + p)
    print("gate: " + ("FAIL" if problems else "pass"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
