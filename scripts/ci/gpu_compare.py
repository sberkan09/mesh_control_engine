#!/usr/bin/env python3
"""Real-GPU vs software render comparison for windowed capture tests.

Takes ctest test names, reads each test's command from `ctest --show-only=json-v1`, runs it twice (real GPU through
SDL's offscreen video driver, then Mesa llvmpipe under Xvfb), and compares the two captured PPM frames. Optional
`name@tick` overrides `-capture_tick`. Prints (set GPUCMP_ASCII=1 for the preview), per frame: mean RGB, stddev (a blank frame has ~0), the share of pixels
that differ by more than 12/255 between the two renderers, and a 64x20 luminance preview of the real-GPU frame.

    gpu_compare.py <build dir> <test>[@tick] ...
"""
import json, os, subprocess, sys, shutil
import numpy as np
from PIL import Image

build = sys.argv[1]
specs = sys.argv[2:]
tests = {t["name"]: t for t in json.loads(subprocess.run(
    ["ctest", "--test-dir", build, "--show-only=json-v1"], capture_output=True, text=True).stdout)["tests"]}
out = "/work/cmp"; os.makedirs(out, exist_ok=True)

def run(cmd, env_extra, wrap):
    env = dict(os.environ, XDG_RUNTIME_DIR="/tmp/xdg", **env_extra)
    return subprocess.run(wrap + cmd, env=env, capture_output=True, text=True, timeout=400)

def frame(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)

for spec in specs:
    name, _, tick = spec.partition("@")
    t = tests.get(name)
    if not t:
        print(f"## {spec}: no such test"); continue
    cmd = list(t["command"]); wd = next((p["value"] for p in t.get("properties", []) if p["name"] == "WORKING_DIRECTORY"), None)
    res = {}
    for mode, env, wrap in (("gpu", {"SDL_VIDEODRIVER": "offscreen"}, []),
                            ("sw", {"LIBGL_ALWAYS_SOFTWARE": "1", "VK_ICD_FILENAMES": "/usr/share/vulkan/icd.d/lvp_icd.json"},
                             ["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24"])):
        c = list(cmd); dst = f"{out}/{name}{('_' + tick) if tick else ''}_{mode}.ppm"
        if "-capture" not in c:
            print(f"## {spec}: command has no -capture"); break
        c[c.index("-capture") + 1] = dst
        if tick and "-capture_tick" in c: c[c.index("-capture_tick") + 1] = tick
        elif tick and "-frames" in c:  # frame count depends on wall-clock catch-up; pin the simulation tick instead
            i = c.index("-frames"); c[i:i + 2] = ["-capture_tick", tick]
        try:
            r = run(c, env, wrap); res[mode] = (dst, r.returncode, (r.stdout + r.stderr)[-300:])
        except Exception as e:
            res[mode] = (dst, -1, str(e))
    for mode, (dst, rc, tail) in res.items():
        print(f"## {spec} [{mode}] rc={rc} frame={'yes' if os.path.exists(dst) else 'NO'}")
        if rc != 0 or not os.path.exists(dst): print("   ", tail.replace("\n", " | ")[-250:])
    if all(os.path.exists(res[m][0]) for m in ("gpu", "sw") if m in res) and len(res) == 2:
        a, b = frame(res["gpu"][0]), frame(res["sw"][0])
        if a.shape != b.shape: print(f"   size differs {a.shape} vs {b.shape}"); continue
        d = np.abs(a - b).max(axis=2)
        print(f"   {a.shape[1]}x{a.shape[0]} gpu mean={a.reshape(-1,3).mean(0).round(1).tolist()} std={a.std():.1f} "
              f"sw mean={b.reshape(-1,3).mean(0).round(1).tolist()} | diff>12: {100*(d>12).mean():.2f}% mean|d|={np.abs(a-b).mean():.2f}")
        if os.environ.get("GPUCMP_ASCII") != "1": continue
        lum = Image.fromarray(a.astype(np.uint8)).convert("L").resize((64, 20))
        ramp = " .:-=+*#%@"
        for row in np.asarray(lum):
            print("   |" + "".join(ramp[int(v) * 10 // 256] for v in row) + "|")
