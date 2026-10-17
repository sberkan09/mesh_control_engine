#!/usr/bin/env python3
"""M-IMPORT3D step 4a: mce import gltf / fbx (tools/mce_cli/mce_import_3d.py) on small hand-written models.

    test_import_3d.py <work dir> [mce_cook]

glTF is checked without any other tool. With mce_cook the FBX route (`mce_cook --dump-fbx-scene`) is checked too, and
the written prefab is cooked for real together with its model.
"""

import base64
import contextlib
import io
import json
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "mce_cli"))
import mce_import  # noqa: E402
import mce_import_3d  # noqa: E402
rotate = mce_import_3d.rotate

failures = []
COOK = sys.argv[2] if len(sys.argv) > 2 else None
K = 683.0 * math.pi  # candela per engine intensity unit


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def run(argv):
    """(exit code, stderr + stdout) of mce_import.main; an exception out of it is a failed check."""
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(err):
            code = mce_import.main(argv)
    except BaseException as e:  # noqa: BLE001
        check(False, f"{argv}: raised {type(e).__name__}: {e}")
        return 99, err.getvalue()
    return code, err.getvalue()


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def near(a, b, eps=1e-4):
    return abs(a - b) <= eps


def near_all(a, b, eps=1e-4):
    return len(a) == len(b) and all(near(x, y, eps) for x, y in zip(a, b))


def quat_axis(axis, deg):
    h = math.radians(deg) / 2
    v = [0, 0, 0]
    v[axis] = math.sin(h)
    return v + [math.cos(h)]


# --- glTF builders -------------------------------------------------------------------------------------------------

TRI = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0) + struct.pack("<3H", 0, 1, 2) + b"\0\0"


def model_doc(nodes, scene_roots=(0,), lights=None, cameras=None, with_mesh=True):
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": list(scene_roots)}], "nodes": nodes}
    if with_mesh:
        doc["buffers"] = [{"byteLength": len(TRI), "uri": "data:application/octet-stream;base64," +
                           base64.b64encode(TRI).decode()}]
        doc["bufferViews"] = [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 6}]
        doc["accessors"] = [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
        doc["meshes"] = [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}]
    if lights is not None:
        doc["extensionsUsed"] = ["KHR_lights_punctual"]
        doc["extensions"] = {"KHR_lights_punctual": {"lights": lights}}
    if cameras is not None:
        doc["cameras"] = cameras
    return doc


def lit(i, **extra):
    n = {"extensions": {"KHR_lights_punctual": {"light": i}}}
    n.update(extra)
    return n


def write_gltf(path, doc):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)


def write_glb(path, doc):
    js = json.dumps(doc).encode()
    js += b" " * (-len(js) % 4)
    body = struct.pack("<II", len(js), 0x4E4F534A) + js
    with open(path, "wb") as f:
        f.write(b"glTF" + struct.pack("<II", 2, 12 + len(body)) + body)


def fresh(work, name):
    d = os.path.join(work, name)
    os.makedirs(d)
    return d


def entity(prefab, name):
    for e in prefab["entities"]:
        if e["name"] == name:
            return e
    return None


def sample_model(d, fname="scene.gltf"):
    """A parent turned 90 degrees about Y at (10, 0, 0), a point light, a spot light, a sun and two cameras below it."""
    q90y = quat_axis(1, 90)
    nodes = [
        {"name": "rig", "translation": [10, 0, 0], "rotation": q90y, "mesh": 0, "children": [1, 2, 3, 4, 5, 6]},
        lit(0, name="Bulb", translation=[0, 2, 0]),
        lit(1, name="Torch", translation=[1, 0, 0], rotation=quat_axis(0, -90)),
        lit(2, name="Sun", rotation=quat_axis(0, 90)),
        {"name": "Main", "camera": 0, "translation": [0, 1, 5]},
        {"camera": 1, "translation": [0, 3, 0]},
        {"name": "Ortho", "camera": 2},
    ]
    lights = [{"type": "point", "color": [1, 0.5, 0.25], "intensity": K * 2, "range": 15},
              {"type": "spot", "intensity": K * 3, "spot": {"innerConeAngle": math.radians(10), "outerConeAngle": math.radians(25)}},
              {"type": "directional", "intensity": 683 * math.pi}]
    cams = [{"type": "perspective", "perspective": {"yfov": math.radians(50), "znear": 0.2, "zfar": 500, "aspectRatio": 1.5}},
            {"type": "perspective", "perspective": {"yfov": math.radians(30), "znear": 0.1}},
            {"type": "orthographic", "orthographic": {"xmag": 1, "ymag": 1, "znear": 0.1, "zfar": 10}}]
    path = os.path.join(d, fname)
    write_gltf(path, model_doc(nodes, lights=lights, cameras=cams))
    return path


def test_gltf_prefab(work):
    d = fresh(work, "gltf")
    src = sample_model(d)
    code, err = run(["gltf", src])
    check(code == 0, "the sample converts: " + err)
    out = os.path.join(d, "scene.mcprefab")
    check(os.path.isfile(out), "the prefab is written beside the model")
    side = load(src + ".mceasset")
    check(side["importer"] == "gltf" and side["guid"], "a model with no sidecar gets one")
    p = load(out)
    check(p["format"] == "mce-prefab" and p["version"] == 1 and p["name"] == "scene" and p["guid"], "prefab header")
    root = p["entities"][0]
    check(root["name"] == "scene" and root["components"]["MeshRef"]["mesh"] == "guid:" + side["guid"], "root: MeshRef to the model")
    check(root["components"]["LocalTransform"] == {} and "ChildOf" not in root["components"], "root at the origin")
    check([e["name"] for e in p["entities"]] == ["scene", "Bulb", "Torch", "Main", "Camera 2"],
          "children in node order, unnamed ones numbered: " + str([e["name"] for e in p["entities"]]))
    check(all(e["components"]["ChildOf"] == {"parent": "$0"} for e in p["entities"][1:]), "children hang off the root")
    bulb = entity(p, "Bulb")["components"]
    # rig turns +Z toward +X (90 about Y): the local (0, 2, 0) stays (0, 2, 0), then +10 in x.
    check(near_all(bulb["LocalTransform"]["position"], [10, 2, 0]), f"world position of the bulb: {bulb['LocalTransform']}")
    check(near_all(bulb["LocalTransform"]["rotation"], quat_axis(1, 90)), "world rotation of the bulb")
    pl = bulb["PointLight3D"]
    check(near(pl["intensity"], 2.0, 1e-3) and pl["radius"] == 15 and near_all(pl["color"], [1, 0.5, 0.25, 1]),
          f"point light: candela / (683 pi), the file's range, colour: {pl}")
    torch = entity(p, "Torch")["components"]
    check(near_all(torch["LocalTransform"]["position"], [10, 0, -1]), f"torch position (local +x turns to -z): {torch['LocalTransform']}")
    sp = torch["SpotLight3D"]
    check(near(sp["intensity"], 3.0, 1e-3) and near(sp["outer_deg"], 25, 1e-3) and near(sp["inner_deg"], 10, 1e-3) and
          sp["direction"] == [0, 0, -1], f"spot light: cone half-angles in degrees, aims down local -Z: {sp}")
    check(near(sp["radius"], 10 * math.sqrt(3), 1e-2), "no range: 10 * sqrt(intensity) metres")
    main = entity(p, "Main")["components"]
    cam = main["Camera3DComponent"]
    check(near(cam["fov_deg"], 50, 1e-3) and near(cam["near_z"], 0.2) and cam["far_z"] == 500 and "primary" not in cam,
          f"camera: yfov degrees, planes, the first is primary: {cam}")
    check(near_all(main["LocalTransform"]["position"], [15, 1, 0]), f"camera world position: {main['LocalTransform']}")
    second = entity(p, "Camera 2")["components"]["Camera3DComponent"]
    check(second["primary"] == 0 and second["far_z"] == 1000, f"the second camera is not primary; no zfar = 1000: {second}")
    check("Sun" not in [e["name"] for e in p["entities"]] and "Ortho" not in [e["name"] for e in p["entities"]], "no entity for the sun or the orthographic camera")
    check("directional light 'Sun'" in err and "Environment3D.sun_direction" in err, "the sun is a named warning: " + err)
    check("[0, 1, 0]" in err, "the sun's direction is reported (90 deg about X turns -Z to +Y, then the parent: [0, 1, 0] -> world)")
    check("sun_intensity about 1 " in err or "sun_intensity about 1 (" in err, "the suggested sun intensity is reported: " + err)
    check("'Ortho' is 'orthographic'" in err, "an orthographic camera is a named warning")
    check("infinite far plane" in err, "a camera with no zfar is warned about")


def test_gltf_directions(work):
    """The sun's direction is the node's -Z in world space."""
    d = fresh(work, "gltf_sun")
    nodes = [lit(0, name="Sun", rotation=quat_axis(0, 90))]
    write_gltf(os.path.join(d, "s.gltf"), model_doc(nodes, lights=[{"type": "directional"}], with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "s.gltf")])
    check(code == 0 and "[0, 1, 0]" in err and "(1 lux" in err, "sun direction (0, 1, 0) from a +90 turn about X: " + err)
    check(len(load(os.path.join(d, "s.mcprefab"))["entities"]) == 1, "only the root remains")


def test_gltf_transforms(work):
    d = fresh(work, "gltf_tf")
    m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1]  # column-major translation (3, 4, 5)
    nodes = [{"matrix": m, "children": [1, 2]},
             lit(0, name="A", matrix=[0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1]),  # 90 about Y, via a matrix
             lit(0, name="B", scale=[2, 2, 2], translation=[1, 0, 0])]
    write_gltf(os.path.join(d, "m.gltf"), model_doc(nodes, lights=[{"type": "point"}], with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "m.gltf")])
    check(code == 0, "matrix and TRS nodes convert: " + err)
    p = load(os.path.join(d, "m.mcprefab"))
    a = entity(p, "A")["components"]["LocalTransform"]
    check(near_all(a["position"], [3, 4, 5]) and near_all(a["rotation"], quat_axis(1, 90)), f"matrix node A: {a}")
    b = entity(p, "B")["components"]["LocalTransform"]
    check(near_all(b["position"], [4, 4, 5]) and "scale" not in b and near_all(b["rotation"], [0, 0, 0, 1]),
          f"scale is dropped, position kept: {b}")
    # The sidecar's "scale" multiplies positions (the cook scales every root node).
    with open(os.path.join(d, "m.gltf.mceasset"), "w") as f:
        json.dump({"guid": "11111111-2222-4333-8444-555555555555", "importer": "gltf", "importer_version": 4,
                   "settings": {"scale": 2.0}}, f)
    code, err = run(["gltf", os.path.join(d, "m.gltf")])
    p = load(os.path.join(d, "m.mcprefab"))
    check(code == 0 and near_all(entity(p, "A")["components"]["LocalTransform"]["position"], [6, 8, 10]), "sidecar scale 2 doubles the positions")
    check(p["entities"][0]["components"]["MeshRef"]["mesh"] == "guid:11111111-2222-4333-8444-555555555555", "an adopted model's GUID is used")
    # A negative scale is a named warning.
    nodes = [lit(0, name="M", scale=[-1, 1, 1])]
    write_gltf(os.path.join(d, "mir.gltf"), model_doc(nodes, lights=[{"type": "point"}], with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "mir.gltf")])
    check(code == 0 and "mirrored" in err, "a mirrored node warns: " + err)


def test_gltf_rules(work):
    """Same bytes twice, kept GUIDs, --werror, --out-dir / --name, GLB."""
    d = fresh(work, "gltf_rules")
    src = sample_model(d)
    out = os.path.join(d, "scene.mcprefab")
    run(["gltf", src])
    first, first_side = read_bytes(out), read_bytes(src + ".mceasset")
    code, _ = run(["gltf", src])
    check(code == 0 and read_bytes(out) == first and read_bytes(src + ".mceasset") == first_side, "run twice: identical bytes")
    guid = load(out)["guid"]
    os.remove(src + ".mceasset")
    run(["gltf", src])
    check(load(out)["guid"] == guid, "the prefab keeps its GUID when the sidecar is new")
    check(load(src + ".mceasset")["guid"] == load(out)["entities"][0]["components"]["MeshRef"]["mesh"][5:], "MeshRef follows the new sidecar")
    # Edit one light: only that value changes, the GUID stays.
    doc = load(src)
    doc["extensions"]["KHR_lights_punctual"]["lights"][0]["intensity"] = K * 4
    write_gltf(src, doc)
    run(["gltf", src])
    p = load(out)
    check(p["guid"] == guid and near(entity(p, "Bulb")["components"]["PointLight3D"]["intensity"], 4.0, 1e-3), "re-import keeps the GUID and takes the edit")
    # --werror: the sample warns (sun, orthographic, infinite far); nothing is written.
    d2 = fresh(work, "gltf_werror")
    src2 = sample_model(d2)
    code, err = run(["gltf", src2, "--werror"])
    check(code == 1 and "--werror" in err, "--werror fails on a warning: " + err)
    check(sorted(os.listdir(d2)) == ["scene.gltf"], f"--werror writes nothing, not even the sidecar: {os.listdir(d2)}")
    # --out-dir and --name; a GLB gives the same prefab content.
    glb = os.path.join(d, "scene.glb")
    write_glb(glb, load(src))
    od = os.path.join(d, "prefabs")
    code, err = run(["gltf", glb, "--out-dir", od, "--name", "Level One"])
    check(code == 0 and os.path.isfile(os.path.join(od, "Level_One.mcprefab")), "--out-dir and --name: " + err)
    g = load(os.path.join(od, "Level_One.mcprefab"))
    check([e["components"].get("LocalTransform") for e in g["entities"][1:]] == [e["components"].get("LocalTransform") for e in p["entities"][1:]],
          "a .glb gives the same placement as the .gltf")
    # No lights or cameras: the prefab is just the model.
    d3 = fresh(work, "gltf_plain")
    write_gltf(os.path.join(d3, "p.gltf"), model_doc([{"mesh": 0}]))
    code, err = run(["gltf", os.path.join(d3, "p.gltf"), "--werror"])
    check(code == 0 and len(load(os.path.join(d3, "p.mcprefab"))["entities"]) == 1, "a model with nothing to place still gets its prefab")


def test_gltf_values(work):
    d = fresh(work, "gltf_values")
    nodes = [lit(0, name="Hot"), lit(1, name="Wide"), {"name": "Tele", "camera": 0}]
    lights = [{"type": "point", "intensity": 1e9, "color": [2, 0, 0]},
              {"type": "spot", "spot": {"outerConeAngle": 1.57, "innerConeAngle": 1.0}}]
    cams = [{"type": "perspective", "perspective": {"yfov": 0.01, "znear": 0.0001, "zfar": 1e7}}]
    write_gltf(os.path.join(d, "v.gltf"), model_doc(nodes, scene_roots=(0, 1, 2), lights=lights, cameras=cams, with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "v.gltf")])
    check(code == 0, "out-of-range values convert with warnings: " + err)
    for needle in ("clamped to 100", "colour [2, 0, 0] is outside 0..1", "outer cone", "field of view", "near plane", "far plane"):
        check(needle in err, f"a warning mentions '{needle}': {err}")
    p = load(os.path.join(d, "v.mcprefab"))
    check(entity(p, "Hot")["components"]["PointLight3D"]["intensity"] == 100, "intensity is clamped to the engine's 100")
    check(entity(p, "Wide")["components"]["SpotLight3D"]["outer_deg"] == 89, "a 90 degree cone is clamped to 89")


def test_gltf_review(work):
    """Sidecar safety, a sun and a camera on one node, overflowing transforms, inner cone, far plane, name collisions."""
    # S1: a sidecar that exists but is malformed or has no GUID is an error, never replaced.
    for label, text in (("typo", '{"guid": "abc", "importer": '), ("noguid", '{"importer": "gltf", "settings": {}}'),
                        ("array", "[1]"), ("emptyguid", '{"guid": ""}')):
        d = fresh(work, "side_" + label)
        src = os.path.join(d, "m.gltf")
        write_gltf(src, model_doc([lit(0, name="L")], lights=[{"type": "point"}], with_mesh=False))
        with open(src + ".mceasset", "w") as f:
            f.write(text)
        code, err = run(["gltf", src])
        check(code == 1 and "m.gltf.mceasset" in err and "Traceback" not in err and len(err.strip().splitlines()) == 1,
              f"sidecar '{label}': exit 1, one line naming the sidecar: code {code}, {err.strip()[:300]}")
        check(read_bytes(src + ".mceasset") == text.encode() and sorted(os.listdir(d)) == ["m.gltf", "m.gltf.mceasset"],
              f"sidecar '{label}': left as it was, nothing written")
    # N1: a node with a sun and a camera yields the camera (and the sun warning).
    d = fresh(work, "sun_cam")
    nodes = [lit(0, name="SunCam", camera=0, translation=[1, 2, 3])]
    cams = [{"type": "perspective", "perspective": {"yfov": 0.8, "znear": 0.1, "zfar": 50}}]
    write_gltf(os.path.join(d, "s.gltf"), model_doc(nodes, lights=[{"type": "directional"}], cameras=cams, with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "s.gltf")])
    p = load(os.path.join(d, "s.mcprefab")) if code == 0 else {"entities": []}
    check(code == 0 and "directional light 'SunCam'" in err and entity(p, "SunCam") is not None and
          "Camera3DComponent" in entity(p, "SunCam")["components"], f"a node with a sun and a camera keeps the camera: {err}")
    # N2: scales that overflow when multiplied down a chain are a named error, not 'cannot convert float NaN'.
    d = fresh(work, "overflow")
    chain = [{"scale": [1e9, 1e9, 1e9], "children": [i + 1]} for i in range(60)] + [lit(0, name="End")]
    write_gltf(os.path.join(d, "o.gltf"), model_doc(chain, lights=[{"type": "point"}], with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "o.gltf")])
    check(code == 1 and "not finite" in err and "NaN" not in err and "Traceback" not in err, f"overflowing chain: {err.strip()[:300]}")
    d = fresh(work, "overflow_tr")
    chain = [{"translation": [1e9, 0, 0], "scale": [1e9, 1e9, 1e9], "children": [i + 1]} for i in range(40)] + [lit(0, name="End")]
    write_gltf(os.path.join(d, "o.gltf"), model_doc(chain, lights=[{"type": "point"}], with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "o.gltf")])
    check(code == 1 and "NaN" not in err and "Traceback" not in err, f"overflowing translations: {err.strip()[:300]}")
    # N9: a spot's inner cone beyond the outer one and a far plane behind the near plane are warned about.
    d = fresh(work, "clamps")
    nodes = [lit(0, name="S"), lit(1, name="Neg"), {"name": "C", "camera": 0}]
    lights = [{"type": "spot", "spot": {"innerConeAngle": 0.6, "outerConeAngle": 0.4}},
              {"type": "spot", "spot": {"innerConeAngle": -0.2, "outerConeAngle": 0.4}}]
    cams = [{"type": "perspective", "perspective": {"yfov": 0.8, "znear": 5, "zfar": 2}}]
    write_gltf(os.path.join(d, "c.gltf"), model_doc(nodes, scene_roots=(0, 1, 2), lights=lights, cameras=cams, with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "c.gltf")])
    check(code == 0 and "light 'S': inner cone" in err and "light 'Neg': inner cone" in err and "not beyond the near plane" in err,
          f"inner > outer, negative inner and far <= near warn: {err}")
    p = load(os.path.join(d, "c.mcprefab"))
    check(near(entity(p, "S")["components"]["SpotLight3D"]["inner_deg"], math.degrees(0.4), 1e-3) and
          entity(p, "Neg")["components"]["SpotLight3D"]["inner_deg"] == 0, "the inner cone is clamped into 0..outer")
    # N10: names that collide with a generated "N 2" still get unique entity names.
    d = fresh(work, "names")
    nodes = [lit(0, name="L"), lit(0, name="L"), lit(0, name="L 2"), lit(0, name="L"), lit(0, name="names")]
    write_gltf(os.path.join(d, "names.gltf"), model_doc(nodes, scene_roots=(0, 1, 2, 3, 4), lights=[{"type": "point"}], with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "names.gltf")])
    got = [e["name"] for e in load(os.path.join(d, "names.mcprefab"))["entities"]]
    check(code == 0 and len(set(got)) == len(got) and got[:4] == ["names", "L", "L 2", "L 2 2"], f"unique entity names: {got}")


def test_importer_versions(work):
    """The sidecar a converter writes carries the importer's real version."""
    def version_of(name):
        text = open(os.path.join(ROOT, "src", "engine", "asset", "cook", "src", name), encoding="utf-8").read()
        m = re.search(r"u32 version\(\) const override \{ return (\d+); \}", text)
        return int(m.group(1)) if m else None
    check(version_of("gltf_importer.cpp") == mce_import_3d.GLTF_IMPORTER_VERSION,
          f"GLTF_IMPORTER_VERSION {mce_import_3d.GLTF_IMPORTER_VERSION} equals gltf_importer.cpp version() {version_of('gltf_importer.cpp')}")
    check(version_of("fbx_importer.cpp") == mce_import_3d.FBX_IMPORTER_VERSION,
          f"FBX_IMPORTER_VERSION {mce_import_3d.FBX_IMPORTER_VERSION} equals fbx_importer.cpp version() {version_of('fbx_importer.cpp')}")


# --- untrusted input -----------------------------------------------------------------------------------------------

def expect_error(work, name, data, needle, ext=".gltf"):
    d = fresh(work, "bad_" + name)
    path = os.path.join(d, "x" + ext)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else json.dumps(data).encode())
    code, err = run(["gltf", path])
    check(code == 1 and needle in err and "Traceback" not in err, f"{name}: exit 1 naming '{needle}': code {code}, {err.strip()[:300]}")
    check(sorted(os.listdir(d)) == ["x" + ext], f"{name}: nothing written")


def test_gltf_corrupt(work):
    good = model_doc([lit(0, name="L")], lights=[{"type": "point"}], with_mesh=False)
    expect_error(work, "empty", b"", "malformed")
    expect_error(work, "not_json", b"hello", "malformed")
    expect_error(work, "array", b"[1,2]", "not an object")
    expect_error(work, "nan", b'{"asset":{"version":"2.0"},"x":NaN}', "NaN")
    expect_error(work, "binary", bytes(range(256)), "UTF-8")
    expect_error(work, "no_version", {"nodes": []}, "glTF 2.0")
    expect_error(work, "deep_json", b"[" * 200000, "nested too deeply")
    expect_error(work, "glb_trunc", b"glTF\x02\x00\x00\x00", "truncated")
    expect_error(work, "glb_v1", b"glTF" + struct.pack("<II", 1, 12), "version 1", ".glb")
    expect_error(work, "glb_len", b"glTF" + struct.pack("<II", 2, 9999) + b"\0" * 8, "truncated", ".glb")
    expect_error(work, "glb_nojson", b"glTF" + struct.pack("<II", 2, 20) + struct.pack("<II", 0, 0x004E4942), "no JSON chunk", ".glb")
    expect_error(work, "glb_big", b"glTF" + struct.pack("<II", 2, 20) + struct.pack("<II", 0xFFFFFFF0, 0x4E4F534A), "over the limit", ".glb")
    expect_error(work, "glb_run", b"glTF" + struct.pack("<II", 2, 40) + struct.pack("<II", 100, 0x4E4F534A) + b"{}" * 10, "past the end", ".glb")
    cyc = model_doc([{"children": [1]}, {"children": [0]}], with_mesh=False)
    expect_error(work, "cycle", cyc, "reached twice")
    dup = model_doc([{"children": [1, 1]}, {}], with_mesh=False)
    expect_error(work, "two_parents", dup, "reached twice")
    expect_error(work, "range", model_doc([{"children": [7]}], with_mesh=False), "out of range")
    bad_light = json.loads(json.dumps(good))
    bad_light["nodes"][0]["extensions"]["KHR_lights_punctual"]["light"] = 3
    expect_error(work, "light_index", bad_light, "light 3")
    bad_type = json.loads(json.dumps(good))
    bad_type["extensions"]["KHR_lights_punctual"]["lights"][0]["type"] = "area"
    expect_error(work, "light_type", bad_type, "'area'")
    bad_tr = json.loads(json.dumps(good))
    bad_tr["nodes"][0]["translation"] = [1, "x", 3]
    expect_error(work, "translation", bad_tr, "translation")
    bad_q = json.loads(json.dumps(good))
    bad_q["nodes"][0]["rotation"] = [0, 0, 0, 0]
    expect_error(work, "zero_quat", bad_q, "zero length")
    huge = json.loads(json.dumps(good))
    huge["nodes"][0]["translation"] = [1e300, 0, 0]
    expect_error(work, "huge", huge, "finite")
    bad_cam = model_doc([{"camera": 0}], cameras=[{"type": "perspective", "perspective": {"yfov": 5, "znear": 0.1}}], with_mesh=False)
    expect_error(work, "yfov", bad_cam, "yfov")
    bad_scene = model_doc([{}], with_mesh=False)
    bad_scene["scene"] = 4
    expect_error(work, "scene", bad_scene, "scene")
    # Limits: too many nodes, a tree deeper than the cap.
    many = model_doc([{} for _ in range((1 << 18) + 1)], with_mesh=False)
    expect_error(work, "node_count", many, "nodes, over the limit")
    deep = model_doc([{"children": [i + 1]} for i in range(4100)] + [{}], with_mesh=False)
    expect_error(work, "depth", deep, "deeper than the limit")
    # A valid deep-enough chain (no recursion in the walker) still works.
    d = fresh(work, "chain")
    chain = [{"children": [i + 1]} for i in range(3000)] + [lit(0, name="End", translation=[1, 0, 0])]
    write_gltf(os.path.join(d, "c.gltf"), model_doc(chain, lights=[{"type": "point"}], with_mesh=False))
    code, err = run(["gltf", os.path.join(d, "c.gltf")])
    check(code == 0 and near_all(entity(load(os.path.join(d, "c.mcprefab")), "End")["components"]["LocalTransform"]["position"], [1, 0, 0]),
          "a 3000-deep chain converts without recursion: " + err[:200])
    # Wrong kind of file for the tool.
    d = fresh(work, "wrongext")
    open(os.path.join(d, "a.obj"), "w").write("v 0 0 0\n")
    code, err = run(["gltf", os.path.join(d, "a.obj")])
    check(code == 1 and ".gltf or .glb" in err, "the wrong extension is refused: " + err)
    code, err = run(["gltf", os.path.join(d, "missing.glb")])
    check(code == 1 and "no such file" in err, "a missing file is refused: " + err)


def test_gltf_fuzz(work):
    """Seeded byte flips and truncations of a valid GLB: always a clean exit 0 or 1, never an exception."""
    d = fresh(work, "fuzz")
    src = sample_model(d)
    glb = os.path.join(d, "f.glb")
    write_glb(glb, load(src))
    base = read_bytes(glb)
    rng = random.Random(7)
    bad = 0
    for i in range(300):
        data = bytearray(base)
        if i % 3 == 0:
            data = data[:rng.randrange(0, len(data))]
        else:
            for _ in range(rng.randrange(1, 4)):
                data[rng.randrange(len(data))] = rng.randrange(256)
        path = os.path.join(d, "m.glb")
        with open(path, "wb") as f:
            f.write(bytes(data))
        code, err = run(["gltf", path, "--out-dir", os.path.join(d, "o")])
        if code not in (0, 1) or "Traceback" in err:
            bad += 1
    check(bad == 0, f"{bad} of 300 mutated files did not end in a clean exit 0 or 1")


# --- FBX (needs mce_cook) ------------------------------------------------------------------------------------------

FBX_HEAD = """; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",2
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",1
		P: "FrontAxisSign", "int", "Integer", "",-1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
"""


def fbx_model(i, name, kind, t, r=(0, 0, 0), s=(1, 1, 1)):
    return (f'\tModel: {i}, "Model::{name}", "{kind}" {{\n\t\tVersion: 232\n\t\tProperties70:  {{\n'
            f'\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",{t[0]},{t[1]},{t[2]}\n'
            f'\t\t\tP: "Lcl Rotation", "Lcl Rotation", "", "A",{r[0]},{r[1]},{r[2]}\n'
            f'\t\t\tP: "Lcl Scaling", "Lcl Scaling", "", "A",{s[0]},{s[1]},{s[2]}\n\t\t}}\n\t}}\n')


def fbx_light(i, ltype, props):
    return (f'\tNodeAttribute: {i}, "NodeAttribute::", "Light" {{\n\t\tTypeFlags: "Light"\n\t\tProperties70:  {{\n'
            f'\t\t\tP: "LightType", "enum", "", "",{ltype}\n{props}\t\t}}\n\t}}\n')


def fbx_doc(lamps):
    """lamps: [(name, translation, rotation, scale, light type, light properties)]; each a model with its light."""
    objs, conn = "", ""
    for k, (name, t, r, sc, ltype, props) in enumerate(lamps):
        objs += fbx_model(100 + 2 * k, name, "Light", t, r, sc) + fbx_light(101 + 2 * k, ltype, props)
        conn += f'\tC: "OO",{100 + 2 * k},0\n\tC: "OO",{101 + 2 * k},{100 + 2 * k}\n'
    return FBX_HEAD + "Objects:  {\n" + objs + "}\nConnections:  {\n" + conn + "}\n"


def fbx_text(extra_objects="", extra_conn=""):
    """Z-up centimetres (3ds Max): a spot light, a point light, a sun and a camera."""
    model, light = fbx_model, fbx_light
    objs = model(1, "Lamp", "Light", (100, 200, 300))
    objs += light(2, 2, '\t\t\tP: "Color", "Color", "", "A",1,0.5,0.25\n\t\t\tP: "Intensity", "Number", "", "A",150\n'
                        '\t\t\tP: "OuterAngle", "Number", "", "A",60\n\t\t\tP: "InnerAngle", "Number", "", "A",30\n')
    objs += model(3, "Cam", "Camera", (0, -500, 100), (90, 0, 0))
    objs += ('\tNodeAttribute: 4, "NodeAttribute::", "Camera" {\n\t\tTypeFlags: "Camera"\n\t\tProperties70:  {\n'
             '\t\t\tP: "FieldOfView", "FieldOfView", "", "A",50\n\t\t\tP: "NearPlane", "double", "Number", "",10\n'
             '\t\t\tP: "FarPlane", "double", "Number", "",10000\n\t\t}\n\t}\n')
    objs += model(5, "Bulb", "Light", (0, 0, 0))
    objs += light(6, 0, '\t\t\tP: "Intensity", "Number", "", "A",100\n')
    objs += model(7, "Sun", "Light", (0, 0, 0))
    objs += light(8, 1, '\t\t\tP: "Intensity", "Number", "", "A",300\n')
    conn = ''.join(f'\tC: "OO",{a},{b}\n' for a, b in ((1, 0), (2, 1), (3, 0), (4, 3), (5, 0), (6, 5), (7, 0), (8, 7)))
    return FBX_HEAD + "Objects:  {\n" + objs + extra_objects + "}\nConnections:  {\n" + conn + extra_conn + "}\n"


def test_fbx(work):
    d = fresh(work, "fbx")
    src = os.path.join(d, "stage.fbx")
    with open(src, "w") as f:
        f.write(fbx_text())
    code, err = run(["fbx", src, "--cook", COOK])
    check(code == 0, "the ASCII FBX converts: " + err)
    p = load(os.path.join(d, "stage.mcprefab"))
    side = load(src + ".mceasset")
    check(side["importer"] == "fbx" and p["entities"][0]["components"]["MeshRef"]["mesh"] == "guid:" + side["guid"], "root MeshRef and the new sidecar")
    check([e["name"] for e in p["entities"]] == ["stage", "Lamp", "Cam", "Bulb"], "entities: " + str([e["name"] for e in p["entities"]]))
    lamp = entity(p, "Lamp")["components"]
    # Z-up centimetres (100, 200, 300) -> +Y up metres (1, 3, -2).
    check(near_all(lamp["LocalTransform"]["position"], [1, 3, -2]), f"the lamp is at Y-up metres: {lamp['LocalTransform']}")
    sp = lamp["SpotLight3D"]
    check(near(sp["intensity"], 1.5, 1e-3) and near(sp["outer_deg"], 30) and near(sp["inner_deg"], 15) and near_all(sp["color"], [1, 0.5, 0.25, 1]),
          f"FBX spot: Intensity / 100, full cone angles halved: {sp}")
    check(near_all(sp["direction"], [0, -1, 0]), f"spot aim axis (the node's local -Y in FBX): {sp['direction']}")
    cam = entity(p, "Cam")["components"]
    c = cam["Camera3DComponent"]
    check(near(c["fov_deg"], 50) and near(c["near_z"], 0.1) and near(c["far_z"], 100), f"camera planes are metres: {c}")
    q = cam["LocalTransform"]["rotation"]
    # The FBX camera looks along file +X, which is +X in the engine: -Z rotated to +X is -90 degrees about Y.
    check(near_all(q, quat_axis(1, -90)) or near_all(q, [-v for v in quat_axis(1, -90)]), f"the camera looks down -Z of its node, toward +X: {q}")
    check(near_all(entity(p, "Bulb")["components"]["PointLight3D"]["color"], [1, 1, 1, 1]) and
          near(entity(p, "Bulb")["components"]["PointLight3D"]["intensity"], 1.0, 1e-3), "FBX point light")
    check("directional light 'Sun'" in err and "Environment3D.sun_direction" in err and "sun_intensity about 3" in err, "the FBX sun warns: " + err)
    first = read_bytes(os.path.join(d, "stage.mcprefab"))
    code, err2 = run(["fbx", src, "--cook", COOK])
    check(code == 0 and read_bytes(os.path.join(d, "stage.mcprefab")) == first, "FBX run twice: identical bytes")
    code, err = run(["fbx", src, "--cook", COOK, "--werror"])
    check(code == 1 and "--werror" in err, "FBX --werror")
    # Corrupt FBX files are a clean error (the cook's reader reports it).
    for name, data in (("trunc", fbx_text().encode()[:300]), ("junk", bytes(range(200))), ("empty", b"")):
        path = os.path.join(d, name + ".fbx")
        with open(path, "wb") as f:
            f.write(data)
        code, err = run(["fbx", path, "--cook", COOK, "--out-dir", os.path.join(d, "o")])
        check(code == 1 and "Traceback" not in err and not os.path.exists(os.path.join(d, "o")), f"corrupt FBX '{name}': exit 1, nothing written: {err.strip()[:200]}")
    code, err = run(["fbx", src, "--cook", os.path.join(d, "nope")])
    check(code == 1 and "mce_cook was not found" in err, "a missing cook is explained: " + err)
    r = subprocess.run([COOK, "--dump-fbx-scene"], capture_output=True, text=True)
    check(r.returncode == 2, "mce_cook --dump-fbx-scene without a file is a usage error")


def test_fbx_review(work):
    """World aim of a spot, decay, switched-off lights, cone fallbacks, mirrored and zero scales."""
    d = fresh(work, "fbx_review")
    spot = '\t\t\tP: "Intensity", "Number", "", "A",100\n\t\t\tP: "OuterAngle", "Number", "", "A",60\n\t\t\tP: "InnerAngle", "Number", "", "A",30\n'
    lamps = [("Aim0", (0, 0, 0), (0, 0, 0), (1, 1, 1), 2, spot),
             ("Aim90", (0, 0, 0), (0, 0, 90), (1, 1, 1), 2, spot),
             ("Linear", (0, 0, 0), (0, 0, 0), (1, 1, 1), 0, '\t\t\tP: "DecayType", "enum", "", "",1\n'),
             ("NoDecay", (0, 0, 0), (0, 0, 0), (1, 1, 1), 0, '\t\t\tP: "DecayType", "enum", "", "",0\n'),
             ("Off", (0, 0, 0), (0, 0, 0), (1, 1, 1), 0, '\t\t\tP: "Intensity", "Number", "", "A",100\n\t\t\tP: "CastLight", "bool", "", "",0\n'),
             ("NoCone", (0, 0, 0), (0, 0, 0), (1, 1, 1), 2, '\t\t\tP: "OuterAngle", "Number", "", "A",0\n'),
             ("Inverted", (0, 0, 0), (0, 0, 0), (1, 1, 1), 2, '\t\t\tP: "OuterAngle", "Number", "", "A",40\n\t\t\tP: "InnerAngle", "Number", "", "A",80\n'),
             ("Area", (0, 0, 0), (0, 0, 0), (1, 1, 1), 3, ""),
             ("Mirror", (0, 0, 0), (0, 0, 0), (-1, 1, 1), 0, "")]
    src = os.path.join(d, "r.fbx")
    with open(src, "w") as f:
        f.write(fbx_doc(lamps))
    code, err = run(["fbx", src, "--cook", COOK])
    check(code == 0, "the review FBX converts: " + err)
    if code != 0:
        return
    p = load(os.path.join(d, "r.mcprefab"))

    def world_aim(name):
        c = entity(p, name)["components"]
        return rotate(c["LocalTransform"]["rotation"], c["SpotLight3D"]["direction"])
    a0, a90 = world_aim("Aim0"), world_aim("Aim90")
    # Z-up file -> +Y up engine: file (x, y, z) is engine (x, z, -y). The light aims down its local -Y (file frame).
    check(near_all(a0, [0, 0, 1], 1e-3), f"spot world aim with no rotation (file -Y is engine +Z): {a0}")
    check(near_all(a90, [1, 0, 0], 1e-3), f"spot world aim after 90 degrees about the file's Z (file +X is engine +X): {a90}")
    check("decay 'linear'" in err and "decay 'none'" in err and "inverse" in err, "linear and no decay both warn: " + err)
    off = entity(p, "Off")["components"]["PointLight3D"]
    check(off["enabled"] == 0 and near(off["intensity"], 1.0, 1e-3) and "switched off" not in err, f"a switched-off light imports with enabled = 0: {off}")
    check("enabled" not in entity(p, "Linear")["components"]["PointLight3D"], "a light that is on carries no enabled field")
    nc = entity(p, "NoCone")["components"]["SpotLight3D"]
    check(near(nc["outer_deg"], 22.5) and "22.5" in err, f"no cone angle: FBX's 45 degree full angle = 22.5 half-angle: {nc}")
    check("light 'Inverted': inner cone" in err and near(entity(p, "Inverted")["components"]["SpotLight3D"]["inner_deg"], 20), "an inner cone past the outer one is clamped with a warning")
    check("area light" in err and entity(p, "Area") is None, "an area light is dropped with a named warning")
    check("'Mirror'" in err and "mirrored" in err, "a negative world scale warns: " + err)
    # A zero scale is an error, as for glTF.
    src = os.path.join(d, "z.fbx")
    with open(src, "w") as f:
        f.write(fbx_doc([("Flat", (0, 0, 0), (0, 0, 0), (0, 1, 1), 0, "")]))
    code, err = run(["fbx", src, "--cook", COOK])
    check(code == 1 and "zero scale" in err and not os.path.exists(os.path.join(d, "z.mcprefab")), f"a zero scale fails: code {code}, {err.strip()[:300]}")


def test_cooks(work):
    """The prefab and its model cook for real (the MeshRef GUID, the light and camera fields all validate)."""
    p = fresh(work, "cook")
    models = os.path.join(p, "assets", "models")
    os.makedirs(models)
    src = os.path.join(models, "room.gltf")
    nodes = [{"name": "room", "mesh": 0, "children": [1, 2, 3]}, lit(0, name="Bulb", translation=[0, 2, 0]),
             lit(1, name="Torch", translation=[2, 1, 0]), {"name": "Eye", "camera": 0, "translation": [0, 1, 6]}]
    lights = [{"type": "point", "intensity": 3000}, {"type": "spot", "intensity": 5000}]
    cams = [{"type": "perspective", "perspective": {"yfov": 0.9, "znear": 0.1, "zfar": 200}}]
    write_gltf(src, model_doc(nodes, lights=lights, cameras=cams))
    code, err = run(["gltf", src])
    check(code == 0, "converts: " + err)
    r = subprocess.run([COOK, p, "--quiet"], capture_output=True, text=True)
    check(r.returncode == 0, f"the model and its prefab cook: exit {r.returncode}\n{r.stdout}{r.stderr}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    work = os.path.abspath(sys.argv[1])
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    tests = [test_gltf_prefab, test_gltf_directions, test_gltf_transforms, test_gltf_rules, test_gltf_values, test_gltf_review,
             test_importer_versions, test_gltf_corrupt, test_gltf_fuzz]
    if COOK:
        tests += [test_fbx, test_fbx_review, test_cooks]
    for t in tests:
        t(work)
    if COOK is None:
        print("note: no mce_cook given; the FBX route and the cook were not checked")
    if failures:
        print(f"{len(failures)} check(s) failed")
        return 1
    print("import_3d: ok" + (" (FBX and cook checked with mce_cook)" if COOK else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
