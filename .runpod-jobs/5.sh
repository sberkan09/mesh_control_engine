#!/usr/bin/env bash
# r7 job 5: GPU cost of each pass at 1920x1080: -perf_capture with the pass on, off, on again, per tier (GL offscreen; High also on sdlgpu).
cd /work/repo; B=build/linux-gcc-release; mkdir -p /work/logs /work/perf /tmp/xdg; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg
cat > /tmp/perf.py <<'PY'
import json, os, subprocess, sys, glob
b = sys.argv[1]
player = next(p for p in glob.glob(f"{b}/**/mce_player", recursive=True) if os.access(p, os.X_OK) and os.path.isfile(p))
kit = next(iter(glob.glob(f"{b}/samples/kit3d_tour/**/assets.mce", recursive=True)))
day = next(iter(glob.glob(f"{b}/samples/11_daynight/**/assets.mce", recursive=True)))
print("player", player, "packs", kit, day, flush=True)
runs = [(kit, "scenes/tour_translucent.mcscene", "r.translucency"), (kit, "scenes/tour_lensrain.mcscene", "r.lens_rain"),
        (kit, "scenes/tour_haze.mcscene", "r.heat_haze"), (kit, "scenes/tour_underwater.mcscene", "r.underwater"),
        (kit, "scenes/tour_shafts.mcscene", "r.sun_shafts"), (day, "scenes/sunrise.mcscene", "r.lens_flare")]
shown = False
def cap(pack, scene, cv, val, tier, be, tag):
    out = f"/work/perf/{os.path.basename(scene).split('.')[0]}_{tier}_{be}_{tag}.mcperf"
    c = [player, "-pack", pack, "-scene", scene, "-window", "1920x1080", "-perf_capture", out, "-sim.fast_forward", "1",
         "-sim.max_catchup", "1", "-r.vsync", "0", "-perf_warmup", "60", "-frames", "600", "-platform.error_box", "0",
         "-pause_on_focus_loss", "0", "-r.quality", tier, "-" + cv, val]
    if be == "gl":
        env = dict(os.environ, SDL_VIDEODRIVER="offscreen", MCE_RHI="gl"); wrap = []
    else:
        env = dict(os.environ, MCE_RHI="sdlgpu", VK_ICD_FILENAMES="/etc/vulkan/icd.d/nvidia_icd.json"); wrap = ["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24"]
    try:
        r = subprocess.run(wrap + c, env=env, capture_output=True, text=True, timeout=240)
    except Exception as e:
        print("   run failed", e); return None
    if not os.path.exists(out):
        print("   no capture rc", r.returncode, (r.stdout + r.stderr)[-300:].replace("\n", " | ")); return None
    j = json.load(open(out)); s = j["summary"]; names = j.get("gpu_stages", [])
    global shown
    if not shown:
        shown = True; print("   header:", {k: v for k, v in j.items() if k not in ("frames", "systems", "summary", "draw_cause_hints", "draw_causes")}, flush=True)
    fr = j["frames"]; n = max(len(fr), 1)
    st = {names[k]: sum(f["gpu_stage_ms"][k] for f in fr if k < len(f["gpu_stage_ms"])) / n for k in range(len(names))}
    return s["gpu_ms"], s["frame_ms"], st, s.get("counters_avg", {})
for be, tiers in (("gl", ["low", "medium", "high"]), ("sdlgpu", ["high"])):
    for pack, scene, cv in runs:
        for tier in tiers:
            res = [cap(pack, scene, cv, v, tier, be, t) for v, t in (("1", "on1"), ("0", "off"), ("1", "on2"))]
            if any(x is None for x in res):
                print(f"PERF {be} {scene} {tier} {cv}: incomplete", flush=True); continue
            g = [x[0] for x in res]; fm = [x[1] for x in res]
            on = (g[0]["avg"] + g[2]["avg"]) / 2
            print(f"PERF {be} {os.path.basename(scene)} {tier} {cv}: gpu avg on {g[0]['avg']:.3f}/{g[2]['avg']:.3f} off {g[1]['avg']:.3f} "
                  f"diff {on - g[1]['avg']:+.3f} | p50 on {g[0]['p50']:.3f}/{g[2]['p50']:.3f} off {g[1]['p50']:.3f} | count {g[0]['count']}/{g[1]['count']} "
                  f"| frame p50 on {fm[0]['p50']:.3f} off {fm[1]['p50']:.3f}", flush=True)
            if res[0][2]:
                d = sorted(((k, (res[0][2].get(k, 0) + res[2][2].get(k, 0)) / 2 - res[1][2].get(k, 0)) for k in set(res[0][2]) | set(res[1][2])), key=lambda kv: -abs(kv[1]))[:4]
                print("     stage diffs (on-off avg ms): " + ", ".join(f"{k} {v:+.3f}" for k, v in d), flush=True)
                if tier == "high" and cv == "r.translucency":
                    print("     stages on: " + ", ".join(f"{k} {v:.3f}" for k, v in res[0][2].items()), flush=True)
PY
timeout 3000 python3 /tmp/perf.py $B 2>&1 | tail -n 120
