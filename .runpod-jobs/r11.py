#!/usr/bin/env python3
"""r11 helpers (job-side only): capture comparison, the shade a pass adds, ASCII look, a base64 JPEG strip, perf summaries."""
import json, os, sys

def load(k):
    import numpy as np
    from PIL import Image
    p = f"/work/cap/{k}.ppm"
    if not os.path.exists(p): return None
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)

def cmp(a, b):
    import numpy as np
    x, y = load(a), load(b)
    if x is None or y is None: print(f"   {a} vs {b}: missing"); return
    if x.shape != y.shape: print(f"   {a} vs {b}: size {x.shape} vs {y.shape}"); return
    d = np.abs(x - y).max(axis=2)
    print(f"   {a:<8} vs {b:<8} {x.shape[1]}x{x.shape[0]} mean {x.reshape(-1,3).mean(0).round(1).tolist()} / "
          f"{y.reshape(-1,3).mean(0).round(1).tolist()} | diff>12: {100*(d>12).mean():.2f}% >2: {100*(d>2).mean():.2f}% mean|d| {np.abs(x-y).mean():.2f} max {d.max()}")

def shade(on, off):
    """How the pass changes the frame: pixels darker / brighter, by how much, and where (top third = mostly sky)."""
    import numpy as np
    x, y = load(on), load(off)
    if x is None or y is None: print(f"   shade {on}/{off}: missing"); return
    lx = x @ np.array([0.2126, 0.7152, 0.0722]); ly = y @ np.array([0.2126, 0.7152, 0.0722])
    dl = lx - ly; h = x.shape[0]
    darker = dl < -2; brighter = dl > 2
    ratio = np.where(ly > 8, lx / np.maximum(ly, 1e-3), 1.0)
    print(f"   shade {on} vs {off}: darker(>2 lum) {100*darker.mean():.2f}% brighter {100*brighter.mean():.3f}% | in darker px: mean dl {dl[darker].mean() if darker.any() else 0:.1f}, "
          f"ratio p50 {np.median(ratio[darker]) if darker.any() else 1:.3f} min {ratio[darker].min() if darker.any() else 1:.3f} | darker by band (top/mid/bottom third) "
          + " / ".join(f"{100*darker[i*h//3:(i+1)*h//3].mean():.1f}%" for i in range(3))
          + f" | max |d| top 15% rows {np.abs(x[:int(h*0.15)]-y[:int(h*0.15)]).max()}")

def ascii(*keys):
    import numpy as np
    from PIL import Image
    ramp = " .:-=+*#%@"
    for k in keys:
        p = f"/work/cap/{k}.ppm"
        if not os.path.exists(p): print(f"## {k} missing"); continue
        im = Image.open(p).convert("RGB"); a = np.asarray(im.resize((96, 27), Image.BOX), dtype=float)
        l = a @ [0.2126, 0.7152, 0.0722]; print(f"## {k} {im.size} mean lum {l.mean():.1f}")
        for row in l: print("".join(ramp[min(9, int(v / 25.6))] for v in row))

def strip(on, off):
    """JPEG strip on | off | 4x |on-off|, 320x180 panels, base64 between IMGSTART / IMGEND (the pod's only way out is its log)."""
    import io, base64
    import numpy as np
    from PIL import Image
    x, y = load(on), load(off)
    if x is None or y is None: print("strip: missing"); return
    d = np.clip(np.abs(x - y) * 4, 0, 255).astype(np.uint8)
    ims = [Image.fromarray(a.astype(np.uint8)).resize((320, 180), Image.LANCZOS) for a in (x, y, d)]
    s = Image.new("RGB", (960, 180))
    for i, im in enumerate(ims): s.paste(im, (320 * i, 0))
    buf = io.BytesIO(); s.save(buf, "JPEG", quality=55); data = base64.b64encode(buf.getvalue()).decode()
    print(f"IMGSTART {on}_{off}.jpg {len(buf.getvalue())}", flush=True)
    for i in range(0, len(data), 100): print(data[i:i+100], flush=True)
    print("IMGEND", flush=True)

def perf(*files):
    import numpy as np
    rows = []
    for f in files:
        try: c = json.load(open(f))
        except Exception as e: print(f"{f}: {e}"); return
        s = c["summary"]; names = c.get("gpu_stages", []); fr = c.get("frames", []); st = {}
        for i, n in enumerate(names):
            v = [x["gpu_stage_ms"][i] for x in fr if i < len(x.get("gpu_stage_ms", [])) and x["gpu_stage_ms"][i] >= 0]
            st[n] = sum(v) / len(v) if v else float("nan")
        g = s.get("gpu_ms", {}); fm = s.get("frame_ms", {})
        rows.append((g.get("avg", float("nan")), g.get("p50", float("nan")), g.get("count", 0), fm.get("avg", float("nan")), st))
    m = lambda i: np.mean([r[i] for r in rows])
    stm = {k: np.mean([r[4][k] for r in rows]) for k in rows[0][4]}
    spread = max(r[0] for r in rows) - min(r[0] for r in rows)
    print(f"gpu avg {m(0):.3f} p50 {m(1):.3f} frame_ms {m(3):.3f} (n={rows[0][2]}, runs {len(rows)}, spread {spread:.3f}) | " +
          " ".join(f"{k} {v:.3f}" for k, v in stm.items() if v == v and v > 0.0005))

if __name__ == "__main__":
    globals()[sys.argv[1]](*sys.argv[2:])
