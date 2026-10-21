#!/usr/bin/env python3
"""M-OPENWORLD D2: the ledger's real packs, fetched, cooked into a project and cooked by mce_cook.

    test_content_real.py <work dir> <mce_cook>

Needs the network (or archives already in MCE_CONTENT_CACHE: the ledger hashes decide whether a file is the right one);
without either the test prints SKIP and passes. Labelled `slow`: the integration run executes it.
"""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools", "mce_cli"))
import mce_content as mc  # noqa: E402
import mce_retarget as rt  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def run(mod, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = mod.main(argv)
    return code, out.getvalue() + err.getvalue()


def main():
    work, cook = sys.argv[1], sys.argv[2]
    shutil.rmtree(work, ignore_errors=True)
    pj = os.path.join(work, "city")
    os.makedirs(os.path.join(pj, "assets"))
    cache = os.environ.get("MCE_CONTENT_CACHE") or os.path.join(work, "cache")
    adds = [("kenney-city-kit-roads", ["models/road-straight.glb", "models/Textures/*"]),
            ("kenney-car-kit", ["models/sedan.glb", "models/Textures/*"]),
            ("kenney-blocky-characters", ["models/character-a.glb", "models/character-b.glb", "models/Textures/*"]),
            ("kenney-impact-sounds", ["audio/footstep_concrete_00[01].ogg"]),
            ("ambientcg-asphalt031", None), ("ambientcg-road007", None)]
    for pack, only in adds:
        argv = ["add", pack, pj, "--cache", cache]
        for o in only or []:
            argv += ["--only", o]
        code, msg = run(mc, argv)
        if code != 0:
            if "download" in msg and "failed" in msg:
                print("SKIP: the network is not reachable and the archive is not cached: " + msg.strip())
                return 0
            check(False, f"add {pack}: {msg}")
            return 1
    code, msg = run(mc, ["verify", pj])
    check(code == 0, "verify passes: " + msg)

    # the node-animated characters: retarget a's clips onto b (same rig, so the map is the identity)
    root = os.path.join(pj, "assets", "third_party", "kenney-blocky-characters", "models")
    names = [n["name"] for n in rt.Gltf(os.path.join(root, "character-a.glb")).json["nodes"]]
    mp = os.path.join(work, "map.json")
    json.dump({n: n for n in names if n not in ("character-a",)}, open(mp, "w"))
    code, msg = run(rt, [os.path.join(root, "character-a.glb"), "--onto", os.path.join(root, "character-b.glb"),
                         "--out", os.path.join(root, "character-b-with-a.glb"), "--map", mp, "--map-only",
                         "--clips", "idle,walk,sprint", "--prefix", "a_"])
    check(code == 0, "node-rig retarget: " + msg)

    r = subprocess.run([cook, pj, "--quiet"], capture_output=True, text=True)
    check(r.returncode == 0, f"mce_cook of the real packs: exit {r.returncode}\n{r.stdout[-1500:]}{r.stderr[-1500:]}")
    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("content_real: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
