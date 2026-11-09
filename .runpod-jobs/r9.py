#!/usr/bin/env python3
"""r9 helpers (job-side only): a scene variant, perf summaries, capture comparison."""
import json, os, shutil, sys
import numpy as np
from PIL import Image

def variant(src, dst, scene, *kv):
    """Copy an assets dir to <dst>/assets and set Environment3D keys (k=int) in <scene>."""
    if os.path.exists(dst): shutil.rmtree(dst)
    shutil.copytree(src, os.path.join(dst, "assets"))
    sp = os.path.join(dst, "assets", scene); doc = json.load(open(sp))
    def walk(o):
        if isinstance(o, dict):
            if isinstance(o.get("Environment3D"), dict):
                for item in kv: k, v = item.split("="); o["Environment3D"][k] = int(v)
            for x in o.values(): walk(x)
        elif isinstance(o, list):
            for x in o: walk(x)
    walk(doc); json.dump(doc, open(sp, "w"), indent=1)

def perf(*files):
    rows = []
    for f in files:
        c = json.load(open(f)); s = c["summary"]; names = c.get("gpu_stages", []); fr = c.get("frames", []); st = {}
        for i, n in enumerate(names):
            v = [x["gpu_stage_ms"][i] for x in fr if i < len(x.get("gpu_stage_ms", [])) and x["gpu_stage_ms"][i] >= 0]
            st[n] = sum(v) / len(v) if v else float("nan")
        g = s.get("gpu_ms", {})
        rows.append((g.get("avg", float("nan")), g.get("p50", float("nan")), g.get("count", 0), st))
    gm = np.mean([r[0] for r in rows]); p50 = np.mean([r[1] for r in rows])
    stm = {k: np.mean([r[3][k] for r in rows]) for k in rows[0][3]}
    spread = max(r[0] for r in rows) - min(r[0] for r in rows)
    print(f"gpu avg {gm:.3f} p50 {p50:.3f} (n={rows[0][2]}, runs {len(rows)}, spread {spread:.3f}) | " +
          " ".join(f"{k} {v:.3f}" for k, v in stm.items() if v == v and v > 0.0005))

def cmp(a, b):
    pa, pb = f"/work/cap/{a}.ppm", f"/work/cap/{b}.ppm"
    if not (os.path.exists(pa) and os.path.exists(pb)): print(f"   {a} vs {b}: missing"); return
    x = np.asarray(Image.open(pa).convert("RGB"), dtype=np.int16); y = np.asarray(Image.open(pb).convert("RGB"), dtype=np.int16)
    if x.shape != y.shape: print(f"   {a} vs {b}: size {x.shape} vs {y.shape}"); return
    d = np.abs(x - y).max(axis=2)
    print(f"   {a:<24} vs {b:<24} {x.shape[1]}x{x.shape[0]} mean {x.reshape(-1,3).mean(0).round(1).tolist()} / "
          f"{y.reshape(-1,3).mean(0).round(1).tolist()} std {x.std():.1f} | diff>12: {100*(d>12).mean():.2f}% mean|d| {np.abs(x-y).mean():.2f} max {d.max()}")

if __name__ == "__main__":
    globals()[sys.argv[1]](*sys.argv[2:])
