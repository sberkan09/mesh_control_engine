#!/usr/bin/env bash
# r7 job 4: gpu_compare (real GPU vs llvmpipe at the pinned tick) and, on the real GPU, pass on vs off.
cd /work/repo; B=build/linux-gcc-release; mkdir -p /work/logs /tmp/xdg; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg
T="sample_kit3d_tour_translucent_gpu sample_kit3d_tour_lensrain_gpu sample_kit3d_tour_haze_gpu sample_kit3d_tour_underwater_gpu sample_kit3d_tour_shafts_gpu sample_daynight_sunrise_gpu"
timeout 1500 python3 scripts/ci/gpu_compare.py $B $T 2>&1 | tail -n 60
cat > /tmp/onoff.py <<'PY'
import json, os, subprocess, sys
import numpy as np
from PIL import Image
b = sys.argv[1]
tests = {t["name"]: t for t in json.loads(subprocess.run(["ctest", "--test-dir", b, "--show-only=json-v1"], capture_output=True, text=True).stdout)["tests"]}
pairs = [("sample_kit3d_tour_translucent_gpu", "r.translucency"), ("sample_kit3d_tour_lensrain_gpu", "r.lens_rain"),
         ("sample_kit3d_tour_haze_gpu", "r.heat_haze"), ("sample_kit3d_tour_underwater_gpu", "r.underwater"),
         ("sample_kit3d_tour_shafts_gpu", "r.sun_shafts"), ("sample_daynight_sunrise_gpu", "r.lens_flare")]
for name, cv in pairs:
    t = tests.get(name)
    if not t: print(name, "missing"); continue
    imgs = {}
    for v in ("1", "0"):
        c = list(t["command"]); dst = f"/work/cmp/{name}_{cv}_{v}.ppm"; c[c.index("-capture") + 1] = dst; c += ["-" + cv, v]
        r = subprocess.run(c, env=dict(os.environ, SDL_VIDEODRIVER="offscreen"), capture_output=True, text=True, timeout=300)
        imgs[v] = dst if os.path.exists(dst) else None
        if r.returncode: print(name, cv, v, "rc", r.returncode, (r.stdout + r.stderr)[-200:].replace("\n", " | "))
    if imgs["1"] and imgs["0"]:
        a = np.asarray(Image.open(imgs["1"]).convert("RGB"), dtype=np.int16); o = np.asarray(Image.open(imgs["0"]).convert("RGB"), dtype=np.int16)
        d = np.abs(a - o).max(axis=2)
        print(f"ONOFF {name} {cv}: on vs off on the GPU diff>12 {100*(d>12).mean():.2f}% mean|d| {np.abs(a-o).mean():.2f} on-mean {a.reshape(-1,3).mean(0).round(1).tolist()} off-mean {o.reshape(-1,3).mean(0).round(1).tolist()}")
PY
timeout 900 python3 /tmp/onoff.py $B 2>&1 | tail -n 30
