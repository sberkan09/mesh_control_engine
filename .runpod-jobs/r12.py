#!/usr/bin/env python3
"""r12 helpers (job-side only): capture comparison, the shade a pass adds, ASCII look, a base64 JPEG strip, perf summaries."""
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

def light(on, off):
    """How the pass changes the frame (caustics add light): pixels brighter / darker, by how much, where, saturation."""
    import numpy as np
    x, y = load(on), load(off)
    if x is None or y is None: print(f"   light {on}/{off}: missing"); return
    w = np.array([0.2126, 0.7152, 0.0722]); lx = x @ w; ly = y @ w; dl = lx - ly; h = x.shape[0]
    br = dl > 2; dk = dl < -2
    ratio = np.where(ly > 8, lx / np.maximum(ly, 1e-3), 1.0)
    pick = lambda a, q: float(np.percentile(a, q)) if a.size else 0.0
    print(f"   light {on} vs {off}: brighter(>2 lum) {100*br.mean():.2f}% darker {100*dk.mean():.3f}% | in brighter px: mean dl {dl[br].mean() if br.any() else 0:.1f}, "
          f"ratio p50 {pick(ratio[br],50):.3f} p90 {pick(ratio[br],90):.3f} max {ratio[br].max() if br.any() else 1:.3f} | brighter by band (top/mid/bottom third) "
          + " / ".join(f"{100*br[i*h//3:(i+1)*h//3].mean():.1f}%" for i in range(3))
          + f" | saturated (any ch 255) on {(x.max(axis=2) >= 255).sum()} off {(y.max(axis=2) >= 255).sum()} | max |d| top 15% rows {np.abs(x[:int(h*0.15)]-y[:int(h*0.15)]).max()}")

def darkgain(on, off):
    """Dark pixels (luma < 90 without the pass: the wall reflected in the water, rock contact shade) that the pass brightens
    by more than 2 levels: count, gain, where (9 x 16 grid of counts)."""
    import numpy as np
    x, y = load(on), load(off)
    if x is None or y is None: print(f"   darkgain {on}/{off}: missing"); return
    w = np.array([0.2126, 0.7152, 0.0722]); lx = x @ w; ly = y @ w; m = (ly < 90) & (lx - ly > 2); H, W = lx.shape
    print(f"   darkgain {on} vs {off}: dark px {(ly < 90).sum()}, brightened {m.sum()} ({100*m.mean():.2f}% of frame), mean gain {(lx-ly)[m].mean() if m.any() else 0:.1f}, "
          f"rgb {y[m].mean(0).round(1).tolist() if m.any() else []} -> {x[m].mean(0).round(1).tolist() if m.any() else []}")
    g = np.zeros((9, 16), int); ys, xs = np.where(m)
    np.add.at(g, (ys * 9 // H, xs * 16 // W), 1)
    for row in g: print("     " + " ".join(f"{v:4d}" for v in row))

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

def golden_blocks(a, b, label):
    """golden.h's measure (8x8 block mean luma, of full scale) between two PNGs: failing blocks with place and values, a block map."""
    import numpy as np
    from PIL import Image
    if not (os.path.exists(a) and os.path.exists(b)): print(f"## {label}: missing {a} or {b}"); return
    x = np.asarray(Image.open(a).convert("RGB"), dtype=float); y = np.asarray(Image.open(b).convert("RGB"), dtype=float)
    lx = x @ [0.2126, 0.7152, 0.0722]; ly = y @ [0.2126, 0.7152, 0.0722]; h, w = lx.shape
    bx = lx.reshape(h // 8, 8, w // 8, 8).mean((1, 3)); by = ly.reshape(h // 8, 8, w // 8, 8).mean((1, 3))
    d = (bx - by) / 255.0
    pd = np.abs(x - y).max(axis=2)
    print(f"## {label}: {w}x{h}, blocks over 0.5%: {(np.abs(d) >= 0.005).sum()} of {d.size}, worst {100*np.abs(d).max():.3f}%, "
          f"pixels differing >2: {(pd > 2).sum()}, >12: {(pd > 12).sum()}, max {pd.max():.0f}")
    for (r, c) in zip(*np.where(np.abs(d) >= 0.004)):
        blk = pd[r*8:(r+1)*8, c*8:(c+1)*8]
        print(f"   block col {c} row {r} (x {c*8}..{c*8+7}, y {r*8}..{r*8+7}): mean luma {bx[r,c]:.1f} vs {by[r,c]:.1f} ({100*d[r,c]:+.3f}%), "
              f"px >2: {(blk > 2).sum()}, max {blk.max():.0f}; rgb mean {x[r*8:(r+1)*8, c*8:(c+1)*8].reshape(-1,3).mean(0).round(1).tolist()} vs "
              f"{y[r*8:(r+1)*8, c*8:(c+1)*8].reshape(-1,3).mean(0).round(1).tolist()}")
    print("   block map (|d| in 0.1% units, '.' under 0.05%):")
    for row in d: print("   " + "".join(("." if abs(v) < 0.0005 else str(min(9, int(abs(v) * 1000)))) for v in row))

def golden_strip(*paths):
    """Native-size strip of PNGs plus |luma diff| x 8 of the first two, as base64 JPEG."""
    import io, base64
    import numpy as np
    from PIL import Image
    ims = [np.asarray(Image.open(p).convert("RGB"), dtype=float) for p in paths]
    l = [im @ [0.2126, 0.7152, 0.0722] for im in ims]
    d = np.clip(np.abs(l[1] - l[0]) * 8, 0, 255)
    panels = [im.astype(np.uint8) for im in ims] + [np.stack([d] * 3, -1).astype(np.uint8)]
    h, w = panels[0].shape[:2]
    s = Image.new("RGB", (w * len(panels), h))
    for i, p in enumerate(panels): s.paste(Image.fromarray(p), (w * i, 0))
    buf = io.BytesIO(); s.save(buf, "JPEG", quality=90); data = base64.b64encode(buf.getvalue()).decode()
    print(f"IMGSTART golden_strip.jpg {len(buf.getvalue())}", flush=True)
    for i in range(0, len(data), 100): print(data[i:i+100], flush=True)
    print("IMGEND", flush=True)

if __name__ == "__main__":
    globals()[sys.argv[1]](*sys.argv[2:])
