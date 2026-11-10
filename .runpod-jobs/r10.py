#!/usr/bin/env python3
"""r10 helpers (job-side only): a stderr trace of Forward3DStats in the player (not committed to master), a city scene
variant, perf summaries, capture comparison, trace summaries."""
import json, os, re, shutil, sys
def patch_player(path):
    s = open(path).read(); key = "stats3d_ = scene3.stats();\n"
    assert s.count(key) == 1
    add = key + r'''            if (std::getenv("MCE_GI_TRACE")) {
                static unsigned long long gi_n = 0;
                const render::Forward3DStats& g = renderer_.forward_3d().last_stats();
                std::fprintf(stderr, "GITRACE %llu t=%llu px=%.2f pz=%.2f probes=%u draws=%u vis=%u scr=%u rel=%u fill=%d load=%d gi=%d visw=%d\n",
                             gi_n++, static_cast<unsigned long long>(platform::now_us()), static_cast<double>(cam3.position.x),
                             static_cast<double>(cam3.position.z), g.gi_probes, g.gi_draws, g.gi_vis_draws, g.gi_scrolled,
                             g.gi_relocated, g.gi_filled ? 1 : 0, g.gi_loaded ? 1 : 0, g.gi ? 1 : 0, g.gi_visibility ? 1 : 0);
            }
'''
    open(path, "w").write(s.replace(key, add)); print("patched", path)
def variant(src, dst, scene, *kv):
    """Copy an assets dir to <dst>/assets; set GiVisibility3D keys (k=int) in <scene>."""
    if os.path.exists(dst): shutil.rmtree(dst)
    shutil.copytree(src, os.path.join(dst, "assets"))
    sp = os.path.join(dst, "assets", scene); doc = json.load(open(sp)); n = 0
    def walk(o):
        nonlocal n
        if isinstance(o, dict):
            if isinstance(o.get("GiVisibility3D"), dict):
                for item in kv: k, v = item.split("="); o["GiVisibility3D"][k] = int(v)
                n += 1
            for x in o.values(): walk(x)
        elif isinstance(o, list):
            for x in o: walk(x)
    walk(doc); json.dump(doc, open(sp, "w"), indent=1); print("variant: GiVisibility3D sets", n)
def shift_input(src, dst, off, hold_from, hold_to):
    """drive.input shifted by `off` frames (idle first, the volume fills), then the car held forward hold_from..hold_to."""
    off, hold_from, hold_to = int(off), int(hold_from), int(hold_to); out = []
    for ln in open(src):
        m = re.match(r"^(\d+)(?:-(\d+))?\s+(.*)$", ln.strip())
        if not m: continue
        a = int(m.group(1)); b = m.group(2); rest = m.group(3)
        if rest.startswith("Interact") and a == 110: out.append(f"{a+off} Interact"); continue
        if rest.startswith("Move") and a == 130: continue  # replaced by the long hold
        if a >= 340: continue
        out.append(f"{a+off}{('-'+str(int(b)+off)) if b else ''} {rest}")
    out.append(f"{hold_from+off}-{hold_to+off} Move=0,1")
    open(dst, "w").write("\n".join(out) + "\n"); print(open(dst).read())
def perf(*files):
    import numpy as np
    rows = []
    for f in files:
        c = json.load(open(f)); s = c["summary"]; names = c.get("gpu_stages", []); fr = c.get("frames", []); st = {}
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
def cmp(a, b):
    import numpy as np
    from PIL import Image
    pa, pb = f"/work/cap/{a}.ppm", f"/work/cap/{b}.ppm"
    if not (os.path.exists(pa) and os.path.exists(pb)): print(f"   {a} vs {b}: missing"); return
    x = np.asarray(Image.open(pa).convert("RGB"), dtype=np.int16); y = np.asarray(Image.open(pb).convert("RGB"), dtype=np.int16)
    if x.shape != y.shape: print(f"   {a} vs {b}: size {x.shape} vs {y.shape}"); return
    d = np.abs(x - y).max(axis=2)
    print(f"   {a:<26} vs {b:<26} {x.shape[1]}x{x.shape[0]} mean {x.reshape(-1,3).mean(0).round(1).tolist()} / "
          f"{y.reshape(-1,3).mean(0).round(1).tolist()} | diff>12: {100*(d>12).mean():.2f}% mean|d| {np.abs(x-y).mean():.2f} max {d.max()}")
def trace(logfile):
    """Summarise GITRACE lines: fill length, probes/frame, scrolls, speed, backlog."""
    rows = []
    for ln in open(logfile, errors="replace"):
        m = re.match(r"GITRACE (\d+) t=(\d+) px=(\S+) pz=(\S+) probes=(\d+) draws=(\d+) vis=(\d+) scr=(\d+) rel=(\d+) fill=(\d) load=(\d) gi=(\d) visw=(\d)", ln)
        if m: rows.append([float(x) if '.' in x else int(x) for x in m.groups()])
    if not rows: print("no trace"); return
    n = len(rows); t0 = rows[0][1]
    first_gi = next((r[0] for r in rows if r[11]), None)
    print(f"frames {n}, wall {(rows[-1][1]-t0)/1e6:.1f} s ({n/((rows[-1][1]-t0)/1e6):.1f} fps avg); first frame with gi=1: {first_gi}"
          + (f" at {(rows[first_gi][1]-t0)/1e6:.2f} s" if first_gi is not None else ""))
    tot = sum(r[4] for r in rows); print(f"probes captured in total {tot}; frames with probes>0: {sum(1 for r in rows if r[4]>0)}; max/frame {max(r[4] for r in rows)}")
    for lo, hi in ((0, 100), (100, 400), (400, 800), (800, 1200), (1200, 100000)):
        s = [r for r in rows if lo <= r[0] < hi]
        if s: print(f"  frames {lo}-{min(hi,n)}: probes/frame mean {sum(r[4] for r in s)/len(s):.3f} max {max(r[4] for r in s)}, draws/frame mean {sum(r[5] for r in s)/len(s):.0f}, vis draws mean {sum(r[6] for r in s)/len(s):.0f}, scrolled {sum(r[7] for r in s)}, gi on {sum(r[11] for r in s)}")
    scr = [r for r in rows if r[7] > 0]
    print(f"scroll events {len(scr)}, scrolled probes {sum(r[7] for r in scr)}, relocated max {max(r[8] for r in rows)}")
    # speed from the camera positions (3 s windows) and backlog = scrolled - captured since the fill
    import math
    fps = n / ((rows[-1][1]-t0)/1e6)
    cum_s = cum_c = 0; fill_end = first_gi or 0; back = []
    for r in rows:
        cum_s += r[7]
        if r[0] >= fill_end: cum_c += r[4]
        back.append(max(cum_s - cum_c, 0))
    print(f"max backlog (scrolled - captured since fill, probes) {max(back)}; at the end {back[-1]}")
    step = 60
    for i in range(0, n - step, step):
        a, b = rows[i], rows[i+step]; dt = (b[1]-a[1]) / 1e6
        v = math.hypot(b[2]-a[2], b[3]-a[3]) / dt if dt > 0 else 0
        if v > 0.5: print(f"  frame {a[0]}: {v:.1f} m/s (wall), pos ({a[2]:.0f},{a[3]:.0f}), backlog {back[i]}, scrolled in window {sum(x[7] for x in rows[i:i+step])}")
if __name__ == "__main__":
    globals()[sys.argv[1]](*sys.argv[2:])
