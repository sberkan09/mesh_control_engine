#!/usr/bin/env python3
"""SBA-146: mce import spine / dragonbones (tools/mce_cli/mce_import_skel.py) on small hand-written exports.

    test_import_skel.py <work dir> [mce_cook]

With mce_cook every imported skeleton is cooked for real (the engine's own .mcskel2d importer); without it
only the converter's output is checked.
"""

import contextlib
import copy
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "mce_cli"))
import mce_import_skel  # noqa: E402

FIX = os.path.join(HERE, "fixtures")
failures = []
COOK = sys.argv[2] if len(sys.argv) > 2 else None


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def run(argv):
    """(exit code, stderr) of mce_import_skel.main."""
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        code = mce_import_skel.main(argv)
    return code, err.getvalue()


def png(path, w, h):
    raw = b"".join(b"\x00" + bytes([200, 100, 50, 255]) * w for _ in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def project(work, name):
    """An empty project with the atlas page cooked as a texture."""
    p = os.path.join(work, name)
    os.makedirs(os.path.join(p, "assets", "skeletons"))
    os.makedirs(os.path.join(p, "assets", "textures"))
    png(os.path.join(p, "assets", "textures", "page.png"), 128, 128)
    return p


def cooks(p, what):
    """The real cook of the project; True when there is no cook to run."""
    if not COOK:
        return True
    r = subprocess.run([COOK, p, "--quiet"], capture_output=True, text=True)
    check(r.returncode == 0, f"{what} cooks: exit {r.returncode}\n{r.stdout}{r.stderr}")
    return r.returncode == 0


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def by_name(items):
    return {i["name"]: i for i in items}


def near(a, b, eps=1e-4):
    return abs(a - b) <= eps


def spine_doc():
    return load(os.path.join(FIX, "hero_spine.json"))


def write(path, doc):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)


def import_spine(work, name="spine"):
    p = project(work, name)
    out = os.path.join(p, "assets", "skeletons", "hero.mcskel2d")
    code, err = run(["spine", os.path.join(FIX, "hero_spine.json"), "--out", out, "--atlas",
                     os.path.join(FIX, "hero_spine.atlas"), "--texture", os.path.join(p, "assets", "textures", "page.png")])
    return p, out, code, err


def test_spine(work):
    p, out, code, err = import_spine(work)
    check(code == 0, "the Spine fixture converts: " + err)
    check("skin 'gold' is dropped" in err, "the extra skin is warned about")
    check("event 'step'" in err and "payload" in err, "an event payload is warned about")
    check("weighted average" in err, "the weighted deform approximation is warned about")
    check("solid" not in err, "no 'solid quad' warning when --texture is given")
    check(os.path.isfile(os.path.join(p, "assets", "textures", "page.png.mceasset")), "the texture got a sidecar")
    s = load(out)
    check(s["format"] == "mce-skeleton2d" and s["version"] == 1 and s["guid"], "header")
    b = by_name(s["bones"])
    check(list(b) == ["root", "hip", "arm_upper", "arm_lower", "hand_goal"], "bones in order")
    check(near(b["hip"]["y"], 0.5) and near(b["hip"]["length"], 0.2), "pixels become metres at 100 per metre")
    check(b["arm_upper"]["rotation"] == -30 and b["arm_lower"]["scale"] == [1.1, 1], "rotation (CCW degrees) and scale are kept")
    sl = by_name(s["slots"])
    check(list(sl) == ["body", "cape"], "slots keep their draw order")
    check(sl["cape"]["tint"][0] == 0.8, "slot color")
    body = by_name(sl["body"]["attachments"])
    check(body["body"]["rect"] == [0.015625, 0.015625, 0.328125, 0.484375], "atlas rect: normalized, v from the top")
    check(body["body"]["size"] == [0.4, 0.6] and body["body"]["offset"] == [0, 0.3] and body["body"]["rotation"] == 5, "region size, offset, rotation")
    check(near(body["body_hit"]["size"][0], 0.3) and near(body["body_hit"]["offset"][1], 0.29), "a trimmed region sits inside its original")
    check(sl["body"]["attachment"] == "body", "setup attachment")
    cape = sl["cape"]["attachments"][0]
    check(cape["type"] == "mesh" and len(cape["vertices"]) == 4 and cape["triangles"] == [0, 1, 2, 0, 2, 3], "mesh")
    v2 = cape["vertices"][2]["weights"]
    check([w["bone"] for w in v2] == ["hip", "arm_upper"] and v2[0]["weight"] == 0.5 and near(v2[1]["x"], -0.1), "weighted mesh vertices")
    check(cape["vertices"][0]["uv"][0] == 0.015625, "mesh uvs are mapped into the atlas page")
    ik = s["ik"][0]
    check(ik["bones"] == ["arm_upper", "arm_lower"] and ik["type"] == "two_bone" and ik["bend"] == "negative" and ik["mix"] == 0.75,
          "two-bone IK with bend and mix")
    a = by_name(s["animations"])
    rot = a["wave"]["bones"]["arm_upper"]["rotate"]
    check(rot[0]["curve"] == [0.3, 0, 0.7, 1], f"a 4.x absolute bezier is normalized: {rot[0].get('curve')}")
    check(rot[1]["curve"] == "stepped" and "curve" not in rot[2], "stepped and linear keys")
    check(a["wave"]["bones"]["arm_upper"]["translate"][1] == {"t": 1, "x": 0.1, "y": -0.2}, "translate keys in metres")
    check(a["wave"]["bones"]["hip"]["scale"][1] == {"t": 0.5, "x": 1.1, "y": 0.9}, "scale keys")
    check(a["wave"]["slots"]["body"]["attachment"] == [{"t": 0, "name": "body"}, {"t": 0.5, "name": "body_hit"}], "attachment keys")
    col = a["wave"]["slots"]["cape"]["color"]
    check(col[0]["color"] == [1, 1, 1, 1] and col[1]["color"][1] == 0.50196 and "curve" in col[0], "color keys with a curve")
    check(a["wave"]["ik"]["arm_ik"] == [{"t": 0, "mix": 0}, {"t": 0.5, "mix": 1}], "ik mix keys")
    check(a["wave"]["deform"]["cape"]["cape"][1]["offsets"][6:] == [0.1, 0], "deform offsets")
    check(a["wave"]["events"] == [{"t": 0.5, "name": "step"}] and a["wave"]["duration"] == 1 and a["idle"]["duration"] == 1.6,
          "events and durations")
    cooks(p, "the Spine skeleton")
    # a re-import keeps the GUID and gives the same bytes
    with open(out, "rb") as f:
        first = f.read()
    code, _ = run(["spine", os.path.join(FIX, "hero_spine.json"), "--out", out, "--atlas",
                   os.path.join(FIX, "hero_spine.atlas"), "--texture", os.path.join(p, "assets", "textures", "page.png")])
    with open(out, "rb") as f:
        check(code == 0 and f.read() == first, "a re-import gives the same file (GUID kept)")
    # --ppm
    out2 = os.path.join(work, "spine", "ppm.mcskel2d")
    run(["spine", os.path.join(FIX, "hero_spine.json"), "--out", out2, "--ppm", "50"])
    check(near(by_name(load(out2)["bones"])["hip"]["y"], 1.0), "--ppm 50 doubles the metres")


def test_spine_unsupported(work):
    d = os.path.join(work, "unsup")
    os.makedirs(d)
    doc = spine_doc()
    doc["bones"][2]["shearX"] = 10
    doc["bones"][3]["transform"] = "noScale"
    doc["slots"][0]["blend"] = "additive"
    doc["slots"][0]["dark"] = "000000"
    doc["transform"] = [{"name": "tc", "bones": ["arm_lower"], "target": "root"}]
    doc["path"] = [{"name": "pc", "bones": ["arm_lower"], "target": "body"}]
    doc["physics"] = [{"name": "ph", "bone": "arm_lower"}]
    doc["skins"][0]["attachments"]["body"]["bb"] = {"type": "boundingbox", "vertices": [0, 0, 1, 0, 1, 1]}
    w = doc["animations"]["wave"]
    w["drawOrder"] = [{"time": 0.5, "offsets": [{"slot": "body", "offset": 1}]}]
    w["bones"]["arm_upper"]["shear"] = [{"x": 1, "y": 0}]
    w["slots"]["body"]["twoColor"] = [{"light": "ffffffff", "dark": "000000"}]
    w["transform"] = {"tc": [{"mixRotate": 0.5}]}
    w["events"][0]["string"] = "x"
    src = os.path.join(d, "u.json")
    write(src, doc)
    out = os.path.join(d, "u.mcskel2d")
    code, err = run(["spine", src, "--out", out])
    check(code == 0, "unsupported features do not fail the import: " + err)
    for needle in ("shear", "'noScale'", "blends 'additive'", "dark", "transform constraint 'tc'", "path constraint 'pc'",
                   "physics constraint 'ph'", "'boundingbox' attachment", "draw order keys", "'shear' timelines",
                   "'twoColor' timelines", "transform constraint timelines", "payload", "skin 'gold'", "no --texture"):
        check(needle in err, f"warning for '{needle}' in: {err}")
    s = load(out)
    check("path" not in s and "transform" not in s and len(s["ik"]) == 1, "unsupported constraints are not written")
    check(os.path.isfile(out) and run(["spine", src, "--out", os.path.join(d, "strict.mcskel2d"), "--strict"])[0] == 1
          and not os.path.exists(os.path.join(d, "strict.mcskel2d")), "--strict fails and writes nothing")
    p = project(work, "unsup_proj")
    shutil.rmtree(os.path.join(p, "assets", "textures"))  # no --texture: the quads are solid
    shutil.copy(out, os.path.join(p, "assets", "skeletons", "u.mcskel2d"))
    cooks(p, "the skeleton with dropped features")

    # limits and bad input
    big = spine_doc()
    big["bones"] = [{"name": "root"}] + [{"name": f"b{i}", "parent": "root"} for i in range(64)]
    write(src, big)
    code, err = run(["spine", src, "--out", os.path.join(d, "big.mcskel2d")])
    check(code == 1 and "65 bones" in err and "64" in err, "more than 64 bones is an error: " + err)
    many = spine_doc()
    many["ik"] = [dict(many["ik"][0], name=f"ik{i}") for i in range(18)]
    write(src, many)
    code, err = run(["spine", src, "--out", os.path.join(d, "many.mcskel2d")])
    check(code == 0 and "allows 16 constraints" in err and len(load(os.path.join(d, "many.mcskel2d"))["ik"]) == 16,
          "IK beyond 16 is dropped with a warning: " + err)
    with open(src, "wb") as f:
        f.write(b"\x00\x01\x02binary")
    code, err = run(["spine", src])
    check(code == 1 and "binary" in err.lower() or "not JSON" in err, "a binary export is refused: " + err)
    write(src, {"armature": []})
    code, err = run(["spine", src])
    check(code == 1 and "not a Spine JSON" in err, "another tool's JSON is refused: " + err)
    # a 1-bone IK is a CCD aim; a broken chain is dropped
    one = spine_doc()
    one["ik"] = [{"name": "aim", "bones": ["arm_upper"], "target": "hand_goal"},
                 {"name": "bad", "bones": ["arm_upper", "hip"], "target": "hand_goal"},
                 {"name": "intarget", "bones": ["hip"], "target": "arm_lower"}]
    one["animations"]["wave"]["ik"] = {"aim": [{"mix": 0.5}]}
    write(src, one)
    out = os.path.join(d, "one.mcskel2d")
    code, err = run(["spine", src, "--out", out])
    s = load(out)
    check(code == 0 and [c["name"] for c in s["ik"]] == ["aim"] and s["ik"][0]["type"] == "ccd", "1-bone IK is CCD, bad chains are dropped: " + err)
    check("'hip' is not a child of 'arm_upper'" in err and "under the chain's first bone" in err, "dropped chains are explained: " + err)


def test_spine_38(work):
    """Spine 3.8 flavour: flat bezier fields, 'angle', shortest-way rotation."""
    d = os.path.join(work, "s38")
    os.makedirs(d)
    doc = spine_doc()
    doc["skeleton"]["spine"] = "3.8.99"
    doc["animations"] = {"turn": {"bones": {"arm_upper": {"rotate": [
        {"angle": 170, "curve": 0.25, "c2": 0.1, "c3": 0.75, "c4": 0.9}, {"time": 1, "angle": -170}]}}}}
    src = os.path.join(d, "s.json")
    write(src, doc)
    out = os.path.join(d, "s.mcskel2d")
    code, err = run(["spine", src, "--out", out])
    check(code == 0 and "turns the short way" in err, "3.x short-way rotation is unwound with a warning: " + err)
    r = load(out)["animations"][0]["bones"]["arm_upper"]["rotate"]
    check(r[0]["curve"] == [0.25, 0.1, 0.75, 0.9] and r[0]["value"] == 170 and r[1]["value"] == 190, f"3.8 curve and angle: {r}")


def test_dragonbones(work):
    p = project(work, "db")
    out = os.path.join(p, "assets", "skeletons", "hero.mcskel2d")
    code, err = run(["dragonbones", os.path.join(FIX, "hero_dragonbones_ske.json"), "--out", out, "--atlas",
                     os.path.join(FIX, "hero_dragonbones_tex.json"), "--texture", os.path.join(p, "assets", "textures", "page.png")])
    check(code == 0, "the DragonBones fixture converts: " + err)
    check("weighted average" in err, "the weighted deform approximation is warned about")
    s = load(out)
    b = by_name(s["bones"])
    check(near(b["torso"]["y"], 0.5) and b["torso"]["rotation"] == 90, "Y-down becomes Y-up, clockwise becomes counter-clockwise")
    check(near(b["arm_upper"]["y"], -0.1) and b["arm_upper"]["rotation"] == -90, "child bone transform")
    sl = by_name(s["slots"])
    check(sl["head"]["tint"] == [1, 1, 1, 0.5] and sl["head"]["attachment"] == "head_2", "slot color and displayIndex")
    head = sl["head"]["attachments"][0]
    check(near(head["offset"][0], 0.405209, 1e-5) and head["rotation"] == -10 and head["size"] == [0.3, 0.3], "trimmed frame, display transform")
    cape = sl["cape"]["attachments"][0]
    v2 = cape["vertices"][2]["weights"]
    check(near(v2[0]["x"], -1.0) and near(v2[1]["x"], 0.4) and near(v2[1]["y"], 0.1), "mesh weights go to bone-local space from the bind pose")
    ik = s["ik"][0]
    check(ik["bones"] == ["arm_upper", "arm_lower"] and ik["bend"] == "negative" and ik["mix"] == 0.75, "IK chain 1, bend flipped by the Y flip")
    a = by_name(s["animations"])
    w = a["wave"]
    check(w["loop"] is True and a["once"]["loop"] is False, "playTimes")
    rot = w["bones"]["arm_upper"]["rotate"]
    check([k["value"] for k in rot] == [0, 30, 10] and rot[0]["curve"] == [0.3, 0, 0.7, 1] and "curve" not in rot[1],
          f"rotation flips sign and takes the short way round: {rot}")
    check([k["t"] for k in rot] == [0, 0.5, 1], "frames become seconds at the frame rate")
    check(w["bones"]["torso"]["translate"][0] == {"t": 0, "x": 0, "y": 0.04, "curve": "stepped"}, "no tweenEasing is a stepped key")
    check(w["slots"]["head"]["attachment"][1] == {"t": 0.5, "name": "head_2"}, "slot display keys")
    check(w["ik"]["arm_ik"][0]["mix"] == 0 and w["events"] == [{"t": 0, "name": "step"}], "ik weight keys, events")
    check(abs(a["once"]["duration"] - 0.41667) < 1e-5, "duration from frames")
    cooks(p, "the DragonBones skeleton")

    # unsupported content
    d = os.path.join(work, "dbu")
    os.makedirs(d)
    doc = load(os.path.join(FIX, "hero_dragonbones_ske.json"))
    arm = doc["armature"][0]
    arm["bone"][2]["inheritRotation"] = False
    arm["bone"][1]["transform"]["skX"] = -80
    arm["slot"][0]["blendMode"] = "add"
    arm["skin"].append({"name": "alt", "slot": []})
    arm["skin"][0]["slot"][0]["display"].append({"type": "armature", "name": "child"})
    arm["animation"][0]["zOrder"] = {"frame": []}
    arm["animation"][0]["frame"][0]["sound"] = "bang"
    arm["animation"][0]["bone"][0]["frame"][0]["tweenRotate"] = 1
    doc["armature"].append({"name": "second", "bone": [{"name": "r"}]})
    src = os.path.join(d, "u_ske.json")
    write(src, doc)
    code, err = run(["dragonbones", src, "--out", os.path.join(d, "u.mcskel2d")])
    check(code == 0, "unsupported DragonBones features do not fail: " + err)
    for needle in ("inheritRotation", "skew", "blends 'add'", "skin 'alt'", "'armature' display", "z-order", "sound frames",
                   "tweenRotate", "armature 'second'", "no --texture"):
        check(needle in err, f"warning for '{needle}' in: {err}")
    check(run(["dragonbones", src, "--out", os.path.join(d, "s.mcskel2d"), "--strict"])[0] == 1
          and not os.path.exists(os.path.join(d, "s.mcskel2d")), "--strict fails and writes nothing")
    code, err = run(["dragonbones", src, "--out", os.path.join(d, "x.mcskel2d"), "--armature", "second"])
    check(code == 0 and len(load(os.path.join(d, "x.mcskel2d"))["bones"]) == 1, "--armature picks one")
    with open(src, "wb") as f:
        f.write(b"DBDT\x00\x00")
    code, err = run(["dragonbones", src])
    check(code == 1 and ".dbbin" in err, "the binary format is refused: " + err)


def test_mce_dispatch(work):
    mce = os.path.join(ROOT, "tools", "mce_cli", "mce.py")
    d = os.path.join(work, "dispatch")
    os.makedirs(d)
    out = os.path.join(d, "h.mcskel2d")
    r = subprocess.run([sys.executable, mce, "import", "spine", os.path.join(FIX, "hero_spine.json"), "--out", out],
                       capture_output=True, text=True)
    check(r.returncode == 0 and os.path.isfile(out), f"`mce import spine` is wired: {r.stderr}")
    r = subprocess.run([sys.executable, mce, "import", "dragonbones", os.path.join(FIX, "hero_dragonbones_ske.json"), "--out", out],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"`mce import dragonbones` is wired: {r.stderr}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    work = os.path.abspath(sys.argv[1])
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    for t in (test_spine, test_spine_unsupported, test_spine_38, test_dragonbones, test_mce_dispatch):
        t(work)
    if COOK is None:
        print("note: no mce_cook given; the outputs were not cooked")
    if failures:
        print(f"{len(failures)} check(s) failed")
        return 1
    print("import_skeletons: ok" + (" (cooked with mce_cook)" if COOK else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
