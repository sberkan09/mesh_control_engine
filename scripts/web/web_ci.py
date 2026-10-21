"""The web stage of scripts/ci/local_ci.sh (M-WEB step 7): the sample ladder built for the browser and
run in headless Chromium (WebGPU on SwiftShader), the web probes, and the three gates.

    python3 scripts/web/web_ci.py [--skip-build] [--update]

1. The host tools: mce_cook (cooks the packs the web build loads) and the native mce_relay.
2. The web-release preset with every sample, then browser_smoke.cjs on each sample's page and
   probe_smoke.cjs on the input and relay probes.
3. Ember Step shipped for the web (scripts/ship.py --target web): the shell page on a desktop and a
   phone-sized page (probe_smoke.cjs shell), and two gates on its zip:
   - itch.io: the zip uploads as an HTML game unchanged (index.html at the root, flat, under itch.io's
     file count and size limits, every file the page loads in it);
   - cold load: what a browser downloads before the first frame (page, loader, wasm and pack, gzipped
     as a web server sends them) stays under the budget in tests/web/baselines.json.
4. Kindle shipped for the web with --smoke-ticks: the drawn and the headless run in the browser give
   the same state hash, and it equals the web baseline in tests/web/baselines.json. The wasm hash is
   its own baseline: a different compiler and libm than the native builds (docs/milestones/M-WEB.md).

--update rewrites the measured values in the baselines file (a deliberate change to a hash or a size).
Needs the Emscripten SDK in the environment (EMSDK), node with Playwright (NODE_PATH) and Chromium.
"""

import argparse
import gzip
import json
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASELINES = os.path.join(ROOT, "tests", "web", "baselines.json")
HOST = "linux-gcc-release"
PRESET = "web-release"
OUT = os.path.join(ROOT, "build", "web-ci")

# The ladder (samples/README.md) and the probes; each sample's page is <dir>/<target>.html.
SAMPLES = [
    ("01_triangle", "01_triangle"),
    ("sprites_2d", "sprites_2d"),
    ("03_platformer", "sample_03_platformer"),
    ("04_diorama", "sample_04_diorama"),
    ("04_cubes", "sample_04_cubes"),
    ("05_ashen_court", "sample_05_ashen_court"),
]
PROBES = ["mce_input_web_probe", "mce_relay_web_probe", "mce_rtc_web_probe"]

# itch.io's HTML upload limits (itch.io/docs/creators/html5): at most 1000 files and 500 MB
# extracted, names up to 240 characters, index.html at the root.
ITCH_MAX_FILES = 1000
ITCH_MAX_EXTRACTED = 500 * 1000 * 1000
ITCH_MAX_NAME = 240


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=ROOT, **kw)


def check(cmd):
    if run(cmd).returncode != 0:
        sys.exit(f"web: failed: {' '.join(cmd)}")


def capture(cmd):
    """Runs cmd, echoes its output and returns it; exits on failure."""
    print("+ " + " ".join(cmd), flush=True)
    p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(p.stdout, end="", flush=True)
    if p.returncode != 0:
        sys.exit(f"web: failed: {' '.join(cmd)}")
    return p.stdout


def build(skip):
    host = os.path.join(ROOT, "build", HOST)
    if not skip:
        check(["cmake", "--preset", HOST])
        check(["cmake", "--build", "--preset", HOST, "--target", "mce_cook", "mce_relay"])
        # No MCE_ONLY_PROJECT: the samples (a ship of a project below narrows the tree to that project).
        check(["cmake", "--preset", PRESET, f"-DMCE_HOST_COOK={host}/tools/cook/mce_cook",
               f"-DMCE_HOST_BUILD_DIR={host}", "-DMCE_ONLY_PROJECT="])
        check(["cmake", "--build", "--preset", PRESET, "--target", *[t for _, t in SAMPLES], *PROBES])
    return host


def smoke_samples():
    web = os.path.join(ROOT, "build", PRESET)
    os.makedirs(OUT, exist_ok=True)
    for d, target in SAMPLES:
        check(["node", "scripts/web/browser_smoke.cjs", os.path.join(web, "samples", d), target,
               os.path.join(OUT, f"{d}.png"), "3000"])


def smoke_probes(host):
    web = os.path.join(ROOT, "build", PRESET, "src", "engine")
    check(["node", "scripts/web/probe_smoke.cjs", os.path.join(web, "platform", "web_probe"), "input"])
    relay = os.path.join(host, "src", "engine", "net", "relay", "mce_relay")
    check(["node", "scripts/web/probe_smoke.cjs", os.path.join(web, "net", "web_probe"), "relay", relay])
    check(["node", "scripts/web/probe_smoke.cjs", os.path.join(web, "net", "web_probe"), "rtc", relay])


def ship(args):
    out = capture([sys.executable, "scripts/ship.py", *args, "--target", "web", "--out", os.path.join(OUT, "dist"),
                   "--version", "ci"])
    m = re.search(r"^ship: (\S+\.zip) ", out, re.M)
    if not m:
        sys.exit("web: ship.py printed no zip")
    return m.group(1), out


def itch_check(zip_path):
    """The zip uploads to itch.io as an HTML game as it is."""
    problems = []
    with zipfile.ZipFile(zip_path) as z:
        infos = z.infolist()
        names = [i.filename for i in infos]
        if "index.html" not in names:
            problems.append("no index.html at the zip's root")
        if any("/" in n or "\\" in n for n in names):
            problems.append("the zip has folders (the web package is flat)")
        if len(infos) > ITCH_MAX_FILES:
            problems.append(f"{len(infos)} files, itch.io takes at most {ITCH_MAX_FILES}")
        extracted = sum(i.file_size for i in infos)
        if extracted > ITCH_MAX_EXTRACTED:
            problems.append(f"{extracted} bytes extracted, itch.io takes at most {ITCH_MAX_EXTRACTED}")
        problems += [f"name too long: {n}" for n in names if len(n) > ITCH_MAX_NAME]
        page = z.read("index.html").decode("utf-8") if "index.html" in names else ""
        problems += [f"index.html has an unfilled {f} field" for f in re.findall(r"\{\{[A-Z_]+\}\}", page)]
        # Every file the page and the loader ask for, by its exact (case-sensitive) name.
        wanted = set(re.findall(r'src="([^"]+)"', page)) | set(re.findall(r'href="([^"]+)"', page))
        wanted |= set(re.findall(r'"([^"/]+\.wasm)"', page))
        wanted.add("assets.mce")
        problems += [f"index.html loads {w}, not in the zip" for w in sorted(wanted)
                     if not re.match(r"[a-z]+:", w) and w not in names]
    print(f"web: itch.io check of {os.path.basename(zip_path)}: {len(infos)} files, {extracted} bytes extracted, "
          f"{'OK' if not problems else 'FAIL'}")
    for p in problems:
        print(f"    {p}")
    return not problems


def cold_load(stage):
    """Bytes a browser downloads before the first frame: the page, the loader, the wasm and the pack,
    each gzipped (level 6, what a web server sends)."""
    total = 0
    for name in sorted(os.listdir(stage)):
        if name == "index.html" or name == "assets.mce" or name.endswith((".js", ".wasm", ".png")):
            with open(os.path.join(stage, name), "rb") as f:
                total += len(gzip.compress(f.read(), 6))
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-build", action="store_true", help="use the existing web-release tree")
    ap.add_argument("--update", action="store_true", help="write the measured hash and size to the baselines")
    args = ap.parse_args()
    if not os.environ.get("EMSDK"):
        sys.exit("web: no Emscripten SDK in the environment (source <emsdk>/emsdk_env.sh)")
    with open(BASELINES) as f:
        base = json.load(f)
    failed = []

    host = build(args.skip_build)
    smoke_samples()
    smoke_probes(host)

    # Ember Step: the shipped page, the itch.io zip and the cold-load budget.
    cl = base["cold_load"]
    zip_path, out = ship([cl["artifact"], "--no-build"])
    stage = zip_path[:-len(".zip")]
    check(["node", "scripts/web/probe_smoke.cjs", stage, "shell"])
    if not itch_check(zip_path):
        failed.append("itch.io zip")
    size = cold_load(stage)
    budget = cl["budget_bytes"]
    print(f"web: {cl['artifact']} cold load {size / 1048576:.2f} MiB gzipped "
          f"(budget {budget / 1048576:.2f} MiB, baseline {cl['measured_bytes'] / 1048576:.2f} MiB)")
    if size > budget:
        failed.append("cold-load budget")
    cl["measured_bytes"] = size

    # Kindle: the web replay hash, drawn and headless, against its own baseline.
    rp = base["replay"]
    _, out = ship(["--project", rp["project"], "--smoke", "--smoke-ticks", str(rp["ticks"])])
    m = re.search(r"^smoke: state_hash ([0-9a-f]{16})", out, re.M)
    got = m.group(1) if m else None
    print(f"web: {rp['project']} {rp['ticks']} ticks: state_hash {got} (baseline {rp['state_hash']})")
    if got != rp["state_hash"]:
        failed.append("web replay hash")
    if got:
        rp["state_hash"] = got

    if args.update:
        with open(BASELINES, "w", newline="\n") as f:
            json.dump(base, f, indent=2)
            f.write("\n")
        print(f"web: baselines written to {os.path.relpath(BASELINES, ROOT)}")
        failed = [f for f in failed if f == "itch.io zip"]
    if failed:
        sys.exit(f"web: FAIL ({', '.join(failed)})")
    print("web: PASS")


if __name__ == "__main__":
    main()
