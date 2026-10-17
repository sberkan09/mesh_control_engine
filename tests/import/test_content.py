#!/usr/bin/env python3
"""M-OPENWORLD D2: mce content (tools/mce_cli/mce_content.py): the open-licence ledger and its cook scripts.

    test_content.py <work dir> [mce_cook]

Offline: archives are built in code and served through a fake opener. With mce_cook the cooked project is cooked
for real.
"""

import contextlib
import copy
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import subprocess
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools", "mce_cli"))
sys.path.insert(0, HERE)
import mce_content as mc  # noqa: E402
import rig_fixture as rf  # noqa: E402

failures = []
COOK = sys.argv[2] if len(sys.argv) > 2 else None


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def run(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
        code = mc.main(argv)
    return code, out.getvalue(), err.getvalue()


def png(w=4, h=4):
    raw = b"".join(b"\x00" + bytes([200, 100, 50, 255]) * w for _ in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def make_zip(path, members):
    with zipfile.ZipFile(path, "w") as z:
        for name, data in members:
            z.writestr(zipfile.ZipInfo(name, (2026, 10, 17, 0, 0, 0)), data)


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def base_pack(zpath, **kw):
    pack = {
        "id": "test-kit", "title": "Test Kit", "kind": "mixed", "author": "Tester", "license": "CC0-1.0",
        "license_url": "https://creativecommons.org/publicdomain/zero/1.0/", "homepage": "https://example.com/test-kit",
        "retrieved": "2026-10-17", "attribution": "Test Kit by Tester", "tags": ["test"],
        "source": {"kind": "http", "url": "https://example.com/test-kit.zip", "archive": "zip", "sha256": sha(zpath),
                   "size": os.path.getsize(zpath), "license_member": "License.txt"},
        "cook": [{"from": "Models/", "include": ["*.glb", "Textures/*.png"], "to": "models"},
                 {"from": "Tex/", "include": ["*_Color.png", "*_Normal.png"], "to": "textures",
                  "settings": [{"match": "*_Normal.png", "settings": {"srgb": False}}]}],
        "materials": [{"path": "street.mcmat", "textures": {"base_color": "textures/street_Color.png", "normal": "textures/street_Normal.png"},
                       "roughness": 0.8, "uv_scale": [4, 4]}],
    }
    pack.update(kw)
    return pack


def ledger_of(*packs):
    return {"version": 1, "licenses": {
        "CC0-1.0": {"name": "CC0", "url": "https://creativecommons.org/publicdomain/zero/1.0/", "attribution_required": False},
        "CC-BY-4.0": {"name": "CC BY 4.0", "url": "https://creativecommons.org/licenses/by/4.0/", "attribution_required": True}},
        "packs": list(packs)}


def test_real_ledger():
    led = mc.load_ledger()
    errs = mc.validate_ledger(led)
    check(not errs, "the shipped ledger is valid: " + "; ".join(errs[:3]))
    check(len(led["packs"]) >= 10, "the ledger lists the seed packs")
    check(all(p["license"] in ("CC0-1.0", "CC-BY-3.0", "CC-BY-4.0") for p in led["packs"]), "only CC0 / CC-BY packs")
    code, out, err = run(["check"])
    check(code == 0 and "ledger ok" in out, "mce content check passes: " + err)
    code, out, err = run(["list", "--kind", "textures"])
    check(code == 0 and "ambientcg-asphalt031" in out and "kenney" not in out, "list --kind filters")
    code, out, err = run(["list", "--tag", "car", "--json"])
    check(code == 0 and json.loads(out)[0]["id"] == "kenney-car-kit", "list --tag --json")
    code, out, err = run(["info", "kenney-car-kit"])
    check(code == 0 and json.loads(out)["author"] == "Kenney", "info prints the entry")
    code, out, err = run(["info", "nope"])
    check(code == 1 and "no pack 'nope'" in err, "unknown pack is an error")


def test_validation(zpath):
    good = ledger_of(base_pack(zpath))
    check(not mc.validate_ledger(good), "a good ledger validates: " + str(mc.validate_ledger(good)))

    def bad(mutate, needle):
        led = copy.deepcopy(good)
        mutate(led)
        errs = mc.validate_ledger(led)
        check(any(needle in e for e in errs), f"rejected ({needle}): {errs}")

    bad(lambda l: l["packs"][0].update(license="CC-BY-NC-4.0"), "not in the ledger's licence table")
    bad(lambda l: l["licenses"].update({"CC-BY-NC-4.0": {"name": "x", "url": "https://x", "attribution_required": True}}), "licence not admitted")
    bad(lambda l: l["licenses"].update({"CC-BY-SA-4.0": {"name": "x", "url": "https://x", "attribution_required": True}}), "licence not admitted")
    bad(lambda l: l["licenses"]["CC-BY-4.0"].update(attribution_required=False), "requires attribution")
    bad(lambda l: l["packs"][0]["source"].pop("sha256"), "sha256")
    bad(lambda l: l["packs"][0]["source"].update(url="http://example.com/a.zip"), "https")
    bad(lambda l: l["packs"][0].update(license="CC-BY-4.0", attribution=""), "requires an 'attribution'")
    bad(lambda l: l["packs"][0]["cook"][0].update(to="../escape"), "'to'")
    bad(lambda l: l["packs"][0]["cook"][0].update({"from": "/abs"}), "'from'")
    bad(lambda l: l["packs"][0]["cook"][0].update(include=[]), "'include'")
    bad(lambda l: l["packs"][0].update(license_url="ftp://x"), "license_url")
    bad(lambda l: l["packs"][0].update(kind="weird"), "'kind'")
    bad(lambda l: l["packs"][0].update(id="Bad_ID"), "'id'")
    bad(lambda l: l["packs"].append(copy.deepcopy(l["packs"][0])), "duplicate")
    bad(lambda l: l["packs"][0].update(retrieved="yesterday"), "'retrieved'")
    bad(lambda l: l["packs"][0]["source"].update(kind="manual"), "source.page")
    bad(lambda l: l["packs"][0].update(materials=[{"path": "../x.mcmat"}]), "'path'")


def test_fetch(work, zpath):
    pack = base_pack(zpath)
    cache = os.path.join(work, "cache")
    calls = []

    class Resp(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    def opener(req, timeout=0):
        calls.append(req.full_url)
        return Resp(open(zpath, "rb").read())

    p = mc.fetch_pack(pack, mc.cache_dir(cache), opener=opener)
    check(os.path.isfile(p) and sha(p) == pack["source"]["sha256"], "fetch downloads and verifies")
    mc.fetch_pack(pack, mc.cache_dir(cache), opener=opener)
    check(len(calls) == 1, "a verified cache entry is not downloaded again")
    check(calls[0] == "https://example.com/test-kit.zip", "the ledger URL is used")

    wrong = copy.deepcopy(pack)
    wrong["source"]["sha256"] = "0" * 64
    try:
        mc.fetch_pack(wrong, mc.cache_dir(os.path.join(work, "cache2")), opener=opener)
        check(False, "a hash mismatch is refused")
    except mc.ContentError as e:
        check("sha256 is" in str(e) and "changed" in str(e), "a hash mismatch names the problem: " + str(e))
    check(not any(f.endswith((".part", ".zip")) for f in os.listdir(os.path.join(work, "cache2"))), "a bad download leaves nothing in the cache")

    manual = copy.deepcopy(pack)
    manual["source"] = {"kind": "manual", "page": "https://example.com/page", "instructions": "Click download.", "archive": "zip"}
    try:
        mc.fetch_pack(manual, mc.cache_dir(os.path.join(work, "cache3")))
        check(False, "a manual pack cannot be fetched by script")
    except mc.ContentError as e:
        check("--file" in str(e) and "example.com/page" in str(e), "a manual pack says where to go: " + str(e))
    with contextlib.redirect_stderr(io.StringIO()):  # "no pinned sha256 yet" is the point of this case
        got = mc.fetch_pack(manual, mc.cache_dir(os.path.join(work, "cache3")), local_file=zpath)
    check(os.path.isfile(got), "a manual pack takes --file")


def test_add(work, zpath, led_path):
    pj = os.path.join(work, "proj")
    os.makedirs(os.path.join(pj, "assets"))
    with open(os.path.join(pj, "CREDITS.md"), "w") as f:
        f.write("# Credits\n\nMy own notes stay.\n")
    cache = os.path.join(work, "cache")
    args = ["--ledger", led_path, "add", "test-kit", pj, "--cache", cache, "--file", zpath]
    code, out, err = run(args)
    check(code == 0, "add runs: " + err + out)
    root = os.path.join(pj, "assets", "third_party", "test-kit")
    files = sorted(os.path.relpath(os.path.join(d, f), root).replace(os.sep, "/") for d, _, fs in os.walk(root) for f in fs)
    check("models/car.glb" in files and "models/car.glb.mceasset" in files, f"model + sidecar written: {files}")
    check("models/Textures/colormap.png.mceasset" in files, "texture sidecar written")
    check("textures/street_Color.png" in files and "street.mcmat" in files and "LICENSE.txt" in files, "textures, material and licence written")
    check("models/ignored.txt" not in files and "Docs/readme.md" not in files, "unselected members are left out")
    sc = json.load(open(os.path.join(root, "models", "car.glb.mceasset")))
    check(sc["importer"] == "gltf" and sc["importer_version"] == 1, "glb gets the gltf importer")
    nsc = json.load(open(os.path.join(root, "textures", "street_Normal.png.mceasset")))
    check(nsc["importer"] == "texture" and nsc["settings"] == {"srgb": False}, "rule settings reach the sidecar")
    mat = json.load(open(os.path.join(root, "street.mcmat")))
    csc = json.load(open(os.path.join(root, "textures", "street_Color.png.mceasset")))
    check(mat["textures"]["base_color"] == "guid:" + csc["guid"] and mat["textures"]["normal"] == "guid:" + nsc["guid"],
          "the material references the textures' GUIDs")
    check(mat["roughness"] == 0.8 and mat["uv_scale"] == [4, 4], "material factors written")
    check(open(os.path.join(root, "LICENSE.txt")).read().startswith("Test kit licence"), "the archive's licence text is copied")
    credits = open(os.path.join(pj, "CREDITS.md")).read()
    check("My own notes stay." in credits and mc.CREDITS_BEGIN in credits and "Test Kit" in credits, "CREDITS.md keeps the user's text and gains the block")
    check("Attribution required" not in credits, "CC0 packs need no attribution list")
    lock = mc.read_lock(pj)
    check(lock["packs"][0]["id"] == "test-kit" and "models/car.glb" in lock["packs"][0]["files"], "the lock records the files")

    # deterministic: a second project gets the same GUIDs; adding twice changes nothing
    code, _, _ = run(args)
    check(code == 0 and json.load(open(os.path.join(root, "models", "car.glb.mceasset")))["guid"] == sc["guid"], "re-adding keeps GUIDs")
    pj2 = os.path.join(work, "proj2")
    os.makedirs(os.path.join(pj2, "assets"))
    run(["--ledger", led_path, "add", "test-kit", pj2, "--cache", cache, "--file", zpath])
    sc2 = json.load(open(os.path.join(pj2, "assets", "third_party", "test-kit", "models", "car.glb.mceasset")))
    check(sc2["guid"] == sc["guid"], "GUIDs depend on the pack and the path, not on the project")
    check(open(os.path.join(pj2, "CREDITS.md")).read().count(mc.CREDITS_BEGIN) == 1, "one generated block")
    credits_before = open(os.path.join(pj, "CREDITS.md")).read()
    run(["credits", pj])
    check(open(os.path.join(pj, "CREDITS.md")).read() == credits_before, "credits is idempotent")

    check(run(["--ledger", led_path, "verify", pj])[0] == 0, "verify passes on a clean project")
    with open(os.path.join(root, "models", "car.glb"), "ab") as f:
        f.write(b"x")
    code, _, err = run(["--ledger", led_path, "verify", pj])
    check(code == 1 and "differs from the recorded hash" in err, "verify catches an edited file: " + err)
    run(["--ledger", led_path, "add", "test-kit", pj, "--cache", cache, "--file", zpath])
    open(os.path.join(pj, "assets", "third_party", "stranger.glb"), "wb").write(b"?")
    code, _, err = run(["--ledger", led_path, "verify", pj])
    check(code == 1 and "stranger.glb: not recorded" in err, "verify catches an unrecorded file: " + err)
    os.remove(os.path.join(pj, "assets", "third_party", "stranger.glb"))
    os.remove(os.path.join(root, "textures", "street_Color.png"))
    code, _, err = run(["--ledger", led_path, "verify", pj])
    check(code == 1 and "recorded but missing" in err, "verify catches a missing file")
    run(["--ledger", led_path, "add", "test-kit", pj, "--cache", cache, "--file", zpath])
    lock = mc.read_lock(pj)
    lock["packs"][0]["license"] = "CC-BY-NC-4.0"
    mc.write_lock(pj, lock)
    code, _, err = run(["--ledger", led_path, "verify", pj])
    check(code == 1 and "not an admitted open licence" in err, "verify refuses a non-open licence in the lock")

    # --only and --dry-run
    pj3 = os.path.join(work, "proj3")
    os.makedirs(os.path.join(pj3, "assets"))
    code, out, _ = run(["--ledger", led_path, "add", "test-kit", pj3, "--cache", cache, "--file", zpath, "--dry-run"])
    check(code == 0 and "dry run" in out and not os.path.exists(os.path.join(pj3, "assets", "third_party")), "--dry-run writes nothing")
    code, out, _ = run(["--ledger", led_path, "add", "test-kit", pj3, "--cache", cache, "--file", zpath, "--only", "models/car.glb"])
    lock3 = mc.read_lock(pj3)
    check(code == 0 and list(lock3["packs"][0]["files"]) == ["models/car.glb"] and lock3["packs"][0]["materials"] == [], "--only selects files (and skips a material whose textures it left out)")
    code, _, err = run(["--ledger", led_path, "add", "test-kit", pj3, "--cache", cache, "--file", zpath, "--only", "nothing/*"])
    check(code == 1 and "select no archive member" in err, "--only that matches nothing is an error")
    code, _, err = run(["--ledger", led_path, "add", "test-kit", os.path.join(work, "no-such"), "--cache", cache, "--file", zpath])
    check(code == 1 and "no assets/ folder" in err, "add into a non-project is an error")
    return pj


def test_cc_by(work, zpath):
    pack = base_pack(zpath, license="CC-BY-4.0", license_url="https://creativecommons.org/licenses/by/4.0/")
    led = ledger_of(pack)
    lp = os.path.join(work, "ccby.json")
    json.dump(led, open(lp, "w"))
    pj = os.path.join(work, "ccby_proj")
    os.makedirs(os.path.join(pj, "assets"))
    code, _, err = run(["--ledger", lp, "add", "test-kit", pj, "--cache", os.path.join(work, "cache"), "--file", zpath])
    credits = open(os.path.join(pj, "CREDITS.md")).read()
    check(code == 0 and "Attribution required" in credits and "Test Kit by Tester" in credits, "CC-BY content lists its attribution: " + err)


def test_zip_slip(work):
    evil = os.path.join(work, "evil.zip")
    make_zip(evil, [("Models/../../escape.glb", b"x"), ("License.txt", b"l")])
    pack = base_pack(evil)
    try:
        mc.plan_members(pack, zipfile.ZipFile(evil).namelist())
        check(False, "a member that escapes is refused")
    except mc.ContentError as e:
        check("escapes" in str(e), "zip-slip refused")


def test_single_file(work):
    f = os.path.join(work, "sky.hdr")
    open(f, "wb").write(b"#?RADIANCE\n")
    pack = {"id": "sky-file", "title": "Sky", "kind": "hdri", "author": "A", "license": "CC0-1.0", "license_url": "https://example.com/l",
            "homepage": "https://example.com", "retrieved": "2026-10-17", "tags": [],
            "source": {"kind": "http", "url": "https://example.com/sky.hdr", "archive": "file", "filename": "sky_1k.hdr",
                       "sha256": sha(f), "size": os.path.getsize(f)},
            "cook": [{"from": "", "include": ["*.hdr"], "to": "hdri"}]}
    lp = os.path.join(work, "single.json")
    json.dump(ledger_of(pack), open(lp, "w"))
    pj = os.path.join(work, "single_proj")
    os.makedirs(os.path.join(pj, "assets"))
    code, _, err = run(["--ledger", lp, "add", "sky-file", pj, "--cache", os.path.join(work, "cache"), "--file", f])
    check(code == 0 and os.path.isfile(os.path.join(pj, "assets", "third_party", "sky-file", "hdri", "sky_1k.hdr")), "a single-file pack is cooked under its filename: " + err)
    check(os.path.isfile(os.path.join(pj, "assets", "third_party", "sky-file", "LICENSE.txt")), "a pack without a licence file gets a generated LICENSE.txt")


def test_cook(work, pj):
    if not COOK:
        return
    r = subprocess_run([COOK, pj, "--quiet"])
    check(r.returncode == 0, f"the cooked-in content cooks: exit {r.returncode}\n{r.stdout}{r.stderr}")


def subprocess_run(argv):
    return subprocess.run(argv, capture_output=True, text=True)


def main():
    work = sys.argv[1]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    glb = rf.build_glb(rf.humanoid({k: k for k in ("hips", "spine", "thigh_l", "thigh_r")}, {"hips": (0, 1, 0), "spine": (0, .2, 0), "thigh_l": (.1, -.05, 0)}))
    zpath = os.path.join(work, "kit.zip")
    make_zip(zpath, [("License.txt", b"Test kit licence\n"), ("Models/car.glb", glb), ("Models/Textures/colormap.png", png()),
                     ("Models/ignored.txt", b"no"), ("Docs/readme.md", b"no"),
                     ("Tex/street_Color.png", png(8, 8)), ("Tex/street_Normal.png", png(8, 8)), ("Tex/street_Rough.png", png(8, 8))])
    test_real_ledger()
    test_validation(zpath)
    test_fetch(work, zpath)
    lp = os.path.join(work, "ledger.json")
    json.dump(ledger_of(base_pack(zpath)), open(lp, "w"))
    pj = test_add(work, zpath, lp)
    test_cc_by(work, zpath)
    test_zip_slip(work)
    test_single_file(work)
    pj_cook = os.path.join(work, "cook_proj")
    os.makedirs(os.path.join(pj_cook, "assets"))
    if run(["--ledger", lp, "add", "test-kit", pj_cook, "--cache", os.path.join(work, "cache"), "--file", zpath])[0] == 0:
        test_cook(work, pj_cook)
    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("content: ok" + (" (cooked with mce_cook)" if COOK else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
