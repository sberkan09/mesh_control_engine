#!/usr/bin/env python3
"""r8 helpers (job-side only): scene variants, perf summaries, ladder metrics."""
import base64, io, json, math, os, shutil, sys
import numpy as np
from PIL import Image

def variant(src, dst, scene, *kv):
    """Copy an assets dir to <dst>/assets and set Environment3D keys (k=v) in <scene>; mat:<file>=<rough> sets a material."""
    if os.path.exists(dst): shutil.rmtree(dst)
    shutil.copytree(src, os.path.join(dst, "assets"))
    sp = os.path.join(dst, "assets", scene)
    doc = json.load(open(sp))
    for item in kv:
        k, v = item.split("=")
        if k.startswith("mat:"):
            mp = os.path.join(dst, "assets", k[4:]); m = json.load(open(mp)); m["roughness"] = float(v)
            json.dump(m, open(mp, "w"), indent=1); continue
        def walk(o):
            if isinstance(o, dict):
                if isinstance(o.get("Environment3D"), dict): o["Environment3D"][k] = float(v)
                for x in o.values(): walk(x)
            elif isinstance(o, list):
                for x in o: walk(x)
        walk(doc)
    json.dump(doc, open(sp, "w"), indent=1)

def perf(*files):
    rows = []
    for f in files:
        c = json.load(open(f)); s = c["summary"]; names = c.get("gpu_stages", [])
        fr = c.get("frames", []); st = {}
        for i, n in enumerate(names):
            v = [x["gpu_stage_ms"][i] for x in fr if i < len(x.get("gpu_stage_ms", [])) and x["gpu_stage_ms"][i] >= 0]
            st[n] = sum(v) / len(v) if v else float("nan")
        g = s.get("gpu_ms", {})
        rows.append((g.get("avg", float("nan")), g.get("p50", float("nan")), g.get("count", 0), st))
    gm = np.mean([r[0] for r in rows]); p50 = np.mean([r[1] for r in rows])
    keys = [k for k in rows[0][3]]
    stm = {k: np.mean([r[3][k] for r in rows]) for k in keys}
    spread = max(r[0] for r in rows) - min(r[0] for r in rows)
    print(f"gpu avg {gm:.3f} p50 {p50:.3f} (n={rows[0][2]}, runs {len(rows)}, spread {spread:.3f}) | " +
          " ".join(f"{k} {v:.3f}" for k, v in stm.items() if v == v and v > 0.0005))

def lum(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return a, a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)

def masks(w, h):
    """Floor strips of tour_reflect (camera (0, 2.7, 7.2), pitch -17 deg, fov 60): ray to y=0 per pixel."""
    th = math.radians(-17.0); t = math.tan(math.radians(30)); asp = w / h
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    nx = (xs + 0.5) / w * 2 - 1; ny = 1 - (ys + 0.5) / h * 2
    fwd = np.array([0, math.sin(th), -math.cos(th)]); up = np.array([0, math.cos(th), math.sin(th)]); rt = np.array([1, 0, 0])
    d = fwd[None, None, :] + nx[..., None] * t * asp * rt + ny[..., None] * t * up
    cam = np.array([0, 2.7, 7.2]); s = -cam[1] / np.where(d[..., 1] < -1e-6, d[..., 1], -1e-6)
    px = cam[0] + s * d[..., 0]; pz = cam[2] + s * d[..., 2]
    out = []
    for k in range(5):
        cx = -3.2 + 1.6 * k
        out.append((np.abs(px - cx) < 0.55) & (pz > -3.6) & (pz < -0.6) & (d[..., 1] < 0))
    return out

def hf(L):
    k = 5; p = np.pad(L, k // 2, mode="edge"); c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    box = (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)
    return np.abs(L - box)

def ladder(off, *imgs):
    """Per strip: refl = mean |L - L_off| (SSR's contribution), hf = SSR-added high-frequency energy (noise), both 0-255."""
    ao, Lo = lum(off); sph = (ao[..., 0] - ao[..., 2]) > 50  # the orange sphere passes in front of some strips
    for _ in range(4): sph = sph | np.roll(sph, 1, 0) | np.roll(sph, -1, 0) | np.roll(sph, 1, 1) | np.roll(sph, -1, 1)
    M = [m & ~sph for m in masks(Lo.shape[1], Lo.shape[0])]; Ho = hf(Lo)
    print("   mask px per strip " + " ".join(str(int(m.sum())) for m in M))
    for p in imgs:
        a, L = lum(p); H = hf(L)
        r = " | ".join(f"{np.abs(L - Lo)[m].mean():5.2f} {(H - Ho)[m].mean():+5.2f}" for m in M)
        print(f"   {os.path.basename(p):<34} refl/hf per strip: {r}")

def jpeg(path, tag):
    a = Image.open(path).convert("RGB"); w, h = a.size
    m = masks(w, h); rows = np.where(np.any(np.any(np.stack(m), 0), 1))[0]
    y0 = max(0, rows.min() - int(0.32 * h)); y1 = min(h, rows.max() + 10)
    a = a.crop((int(0.08 * w), y0, int(0.92 * w), y1)); a = a.resize((560, int(560 * a.size[1] / a.size[0])))
    b = io.BytesIO(); a.save(b, "JPEG", quality=72); s = base64.b64encode(b.getvalue()).decode()
    print(f"##IMG {tag} {len(s)}")
    for i in range(0, len(s), 120): print("##B " + s[i:i + 120])
    print(f"##IMGEND {tag}")

if __name__ == "__main__":
    globals()[sys.argv[1]](*sys.argv[2:])
