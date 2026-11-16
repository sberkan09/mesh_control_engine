#!/usr/bin/env python3
"""mce import gltf / fbx: the lights and cameras of a 3D model as a prefab (M-IMPORT3D step 4).

    mce import gltf <model.gltf|model.glb> [--out-dir d] [--name n] [--werror]
    mce import fbx  <model.fbx>            [--out-dir d] [--name n] [--werror] [--cook path/to/mce_cook]

A model's cook (CHNK_MESH) holds geometry, materials, skins and clips but cannot place entities, so the lights and
cameras that come with the file are written here as <name>.mcprefab:

    entity 0   <name>: MeshRef to the model (its GUID is read from the model's .mceasset sidecar; a model that has no
               sidecar yet gets one here, with a new GUID; a sidecar that exists but cannot be read, or has no
               "guid", is an error and nothing is written), at the origin
    then       one child (ChildOf $0) per light or camera, at the node's WORLD transform in the engine's frame
               (+Y up, right-handed, metres; the sidecar's "scale" multiplies positions, as the cook does):
               PointLight3D, SpotLight3D (direction along the node's local aim axis) or Camera3DComponent (the first
               one primary, the rest not)

Directional lights get no entity (the engine has one sun, Environment3D): a warning names Environment3D.sun_direction
and the direction found. Whatever else cannot be carried is a named warning on stderr (orthographic cameras, area
lights, a mirrored node, clamped values, ...); --werror makes any warning a failure and nothing is written.

Light units (the user guide, section 8.4, has the table). The engine's light intensity is reflected radiance, not
photometric (ADR-0024): a white Lambert surface 1 m from a point light of intensity I reflects I, which is the
physical E / pi. With the usual 683 lm/W:
    glTF point / spot   intensity = candela / (683 * pi)         clamped to 0..100
    glTF directional    lux -> Environment3D.sun_intensity = lux / (683 * pi)   (in the warning, not written)
    FBX                 intensity = the DCC's Intensity / 100 as ufbx reads it (FBX has no photometric unit);
                        cone angles are full angles (halved); a light with no OuterAngle gets FBX's default 45 deg
                        full angle = 22.5 deg half-angle; a light switched off in the file gets enabled = 0
    radius              the file's range (glTF), else the distance where the inverse square falls to 0.01:
                        10 * sqrt(intensity) metres, clamped to 1..200
Colour is taken as linear. Re-importing keeps the prefab's GUID; the output is a pure function of the inputs.

The glTF container is parsed here (JSON or GLB, no dependency). FBX goes through the cook's own reader:
`mce_cook --dump-fbx-scene` prints the lights, cameras and world transforms as JSON, so there is no second FBX parser.
Both inputs are untrusted: sizes, node count and tree depth are bounded and a malformed file fails with a message.
"""

import json
import math
import os
import subprocess
import sys
import uuid

MAX_GLTF_BYTES = 128 << 20      # a .gltf (JSON, maybe with base64 buffers) or the JSON chunk of a .glb
MAX_GLB_JSON_BYTES = 64 << 20
MAX_FBX_BYTES = 256 << 20       # the cook importer's own limit
MAX_NODES = 1 << 18
MAX_DEPTH = 4096
MAX_DUMP_BYTES = 64 << 20
MAX_NAME = 64
LUMENS_PER_WATT = 683.0
MAX_INTENSITY = 100.0
GLTF_IMPORTER_VERSION = 4       # gltf_importer.cpp version(); a sidecar written here matches it
FBX_IMPORTER_VERSION = 1        # fbx_importer.cpp version()
MAX_COORD = 1e9


class Importer:
    def __init__(self, tool):
        self.tool = tool
        self.warnings = []

    def warn(self, msg):
        if msg in self.warnings:
            return
        self.warnings.append(msg)
        print(f"mce import {self.tool}: warning: {msg}", file=sys.stderr)


# --- small math: 4x4 column-major matrices, quaternions [x, y, z, w] -----------------------------------------------

IDENTITY = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]


def finite(v, what):
    if not isinstance(v, (int, float)) or isinstance(v, bool) or not math.isfinite(v) or abs(v) > MAX_COORD:
        raise ValueError(f"{what} is not a finite number")
    return float(v)


def numbers(v, n, what):
    if not isinstance(v, list) or len(v) != n:
        raise ValueError(f"{what} must be an array of {n} numbers")
    return [finite(x, what) for x in v]


def mat_mul(a, b):
    out = [0.0] * 16
    for c in range(4):
        for r in range(4):
            out[c * 4 + r] = sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
    return out


def mat_trs(t, q, s):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    if n == 0:
        raise ValueError("a rotation quaternion has zero length")
    x, y, z, w = x / n, y / n, z / n, w / n
    r = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]
    m = [0.0] * 16
    for c in range(3):
        for rr in range(3):
            m[c * 4 + rr] = r[c * 3 + rr] * s[c]
    m[12], m[13], m[14], m[15] = t[0], t[1], t[2], 1.0
    return m


def quat_from_basis(c0, c1, c2):
    """Quaternion of the rotation whose columns are the unit vectors c0, c1, c2."""
    m00, m10, m20 = c0
    m01, m11, m21 = c1
    m02, m12, m22 = c2
    tr = m00 + m11 + m22
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        q = [(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s]
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2
        q = [0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s]
    elif m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2
        q = [(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s]
    else:
        s = math.sqrt(1.0 + m22 - m00 - m11) * 2
        q = [(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s]
    n = math.sqrt(sum(v * v for v in q))
    q = [v / n for v in q]
    if q[3] < 0:  # one canonical sign: w >= 0
        q = [-v for v in q]
    return q


def decompose(m):
    """(translation, quaternion, scale, mirrored) of a column-major TRS matrix; shear is ignored."""
    cols = [m[0:3], m[4:7], m[8:11]]
    scale = [math.sqrt(sum(v * v for v in c)) for c in cols]
    if min(scale) < 1e-12:
        raise ValueError("a node's world transform has a zero scale")
    u = [[v / scale[i] for v in cols[i]] for i in range(3)]
    det = (u[0][0] * (u[1][1] * u[2][2] - u[1][2] * u[2][1]) - u[1][0] * (u[0][1] * u[2][2] - u[0][2] * u[2][1]) +
           u[2][0] * (u[0][1] * u[1][2] - u[0][2] * u[1][1]))
    mirrored = det < 0
    if mirrored:
        u[0] = [-v for v in u[0]]
        scale[0] = -scale[0]
    return m[12:15], quat_from_basis(*u), scale, mirrored


def world_pose(world, scale, who):
    """(position in metres, rotation, mirrored) of a world matrix; a transform that overflowed is a named error."""
    pos, rot, _scl, mirrored = decompose(world)
    pos = [p * scale for p in pos]
    if any(not math.isfinite(v) or abs(v) > MAX_COORD * 1e6 for v in pos + rot):
        raise ValueError(f"node {who} has a world transform that is not finite (huge scales or offsets multiplied "
                         "down its parents)")
    return pos, rot, mirrored


def rotate(q, v):
    x, y, z, w = q
    # v' = v + 2w (q x v) + 2 q x (q x v)
    cx, cy, cz = y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]
    return [v[0] + 2 * (w * cx + y * cz - z * cy), v[1] + 2 * (w * cy + z * cx - x * cz),
            v[2] + 2 * (w * cz + x * cy - y * cx)]


def num(v, nd=6):
    """A JSON number without float noise or negative zero."""
    v = round(float(v), nd)
    if v == 0:
        return 0
    return int(v) if v == int(v) else v


def vec_text(v):
    return "[" + ", ".join(f"{num(x, 4):g}" for x in v) + "]"


# --- shared prefab building ----------------------------------------------------------------------------------------

def light_radius(intensity, rng):
    if rng is not None:
        return max(0.1, min(200.0, rng))
    return max(1.0, min(200.0, 10.0 * math.sqrt(max(intensity, 0.0))))


def build_prefab(name, guid, mesh_ref, placed):
    """placed: [{"name", "pos", "rot", "components": {...}}] in file order."""
    entities = [{"name": name, "components": {"LocalTransform": {}, "MeshRef": {"mesh": mesh_ref}}}]
    taken, counts = {name}, {}
    for p in placed:
        base = p["name"] or p["fallback"]
        n = counts.get(base, 0) + 1
        ename = base if n == 1 else f"{base} {n}"
        while ename in taken:  # "L", "L", "L 2" must not produce two "L 2"
            n += 1
            ename = f"{base} {n}"
        counts[base] = n
        taken.add(ename)
        comps = dict(p["components"])
        comps["LocalTransform"] = {"position": [num(v) for v in p["pos"]], "rotation": [num(v) for v in p["rot"]]}
        comps["ChildOf"] = {"parent": "$0"}
        entities.append({"name": ename, "components": dict(sorted(comps.items()))})
    return {"format": "mce-prefab", "version": 1, "guid": guid, "name": name, "entities": entities}


def clean_name(v, fallback):
    if not isinstance(v, str) or not v.strip():
        return ""
    s = "".join(c if c.isprintable() else "_" for c in v.strip())
    return s[:MAX_NAME]


def sun_warning(imp, who, direction, lux_or_intensity, glTF, off=False):
    if glTF:
        hint = (f"; Environment3D.sun_intensity about {lux_or_intensity / (LUMENS_PER_WATT * math.pi):.3g} "
                f"({lux_or_intensity:.6g} lux / (683 pi))")
    else:
        hint = f"; Environment3D.sun_intensity about {lux_or_intensity:.3g}"
    imp.warn(f"directional light {who} has no entity (the engine has one sun): set Environment3D.sun_direction to "
             f"{vec_text(direction)}{hint}{'; it is switched off in the file' if off else ''}")


def point_or_spot_components(kind, color, intensity, radius, direction=None, outer=None, inner=None, enabled=True):
    comp = {"color": [num(c) for c in color] + [1], "intensity": num(intensity, 4), "radius": num(radius, 3)}
    if not enabled:
        comp["enabled"] = 0
    if kind == "spot":
        comp["direction"] = [num(v) for v in direction]
        comp["outer_deg"] = num(outer, 4)
        comp["inner_deg"] = num(inner, 4)
        return {"SpotLight3D": comp}
    return {"PointLight3D": comp}


def clamp_inner(imp, who, inner, outer):
    if inner < 0 or inner > outer:
        clamped = min(max(inner, 0.0), outer)
        imp.warn(f"light {who}: inner cone {num(inner, 4):g} deg is outside 0..outer ({num(outer, 4):g}); clamped to {clamped:g}")
        return clamped
    return inner


def camera_components(fov_deg, near, far, primary, imp, who):
    if not 5.0 <= fov_deg <= 170.0:
        clamped = min(170.0, max(5.0, fov_deg))
        imp.warn(f"camera {who}: field of view {num(fov_deg, 4):g} deg is outside the engine's 5..170; clamped to {clamped:g}")
        fov_deg = clamped
    if not 0.001 <= near <= 100.0:
        clamped = min(100.0, max(0.001, near))
        imp.warn(f"camera {who}: near plane {num(near, 4):g} m is outside the engine's 0.001..100; clamped to {clamped:g}")
        near = clamped
    if not far > 0 or far > 100000.0:
        imp.warn(f"camera {who}: far plane {num(far, 4):g} m is missing or outside the engine's 1..100000; using "
                 f"{'1000' if not far > 0 else '100000'}")
        far = 1000.0 if not far > 0 else 100000.0
    elif far < 1.0:
        imp.warn(f"camera {who}: far plane {num(far, 4):g} m is under the engine's 1; clamped to 1")
        far = 1.0
    if far <= near:
        imp.warn(f"camera {who}: far plane {num(far, 4):g} m is not beyond the near plane {num(near, 4):g} m after clamping; "
                 "the engine will not render it")
    comp = {"fov_deg": num(fov_deg, 4), "near_z": num(near, 4), "far_z": num(far, 3)}
    if not primary:
        comp["primary"] = 0
    return {"Camera3DComponent": comp}


# --- glTF ----------------------------------------------------------------------------------------------------------

def read_gltf_json(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(12)
        if head[:4] == b"glTF":
            if len(head) < 12:
                raise ValueError("the GLB header is truncated")
            version = int.from_bytes(head[4:8], "little")
            total = int.from_bytes(head[8:12], "little")
            if version != 2:
                raise ValueError(f"GLB version {version} is not supported (glTF 2.0 only)")
            if total > size:
                raise ValueError(f"the GLB says it is {total} bytes but the file has {size}: truncated")
            ch = f.read(8)
            if len(ch) < 8 or ch[4:8] != b"JSON":
                raise ValueError("the GLB has no JSON chunk first")
            clen = int.from_bytes(ch[0:4], "little")
            if clen > MAX_GLB_JSON_BYTES:
                raise ValueError(f"the GLB's JSON chunk is {clen >> 20} MiB, over the limit of {MAX_GLB_JSON_BYTES >> 20} MiB")
            if 20 + clen > size:
                raise ValueError("the GLB's JSON chunk runs past the end of the file: truncated")
            raw = f.read(clen)
        else:
            if size > MAX_GLTF_BYTES:
                raise ValueError(f"the file is {size >> 20} MiB, over the limit of {MAX_GLTF_BYTES >> 20} MiB for a .gltf")
            raw = head + f.read()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        raise ValueError("the glTF JSON is not valid UTF-8 (is this a glTF file?)") from None

    def bad_constant(c):
        raise ValueError(f"the glTF JSON contains {c}, which JSON does not allow")
    try:
        doc = json.loads(text, parse_constant=bad_constant)
    except RecursionError:
        raise ValueError("the glTF JSON is nested too deeply") from None
    except json.JSONDecodeError as e:
        raise ValueError(f"the glTF JSON is malformed (line {e.lineno}, column {e.colno}): {e.msg}") from None
    if not isinstance(doc, dict):
        raise ValueError("the glTF JSON is not an object")
    asset = doc.get("asset")
    if not isinstance(asset, dict) or not str(asset.get("version", "")).startswith("2"):
        raise ValueError('not a glTF 2.0 file: "asset.version" is missing or is not 2.x')
    return doc


def node_matrix(node, idx):
    if "matrix" in node:
        m = numbers(node["matrix"], 16, f"node {idx} matrix")
        if m[3] != 0 or m[7] != 0 or m[11] != 0 or m[15] != 1:
            raise ValueError(f"node {idx} matrix is not an affine transform")
        return m
    t = numbers(node.get("translation", [0, 0, 0]), 3, f"node {idx} translation")
    q = numbers(node.get("rotation", [0, 0, 0, 1]), 4, f"node {idx} rotation")
    s = numbers(node.get("scale", [1, 1, 1]), 3, f"node {idx} scale")
    return mat_trs(t, q, s)


def gltf_walk(doc):
    """[(node index, world matrix)] of the default scene's nodes in depth-first order; bounded and cycle-checked."""
    nodes = doc.get("nodes", [])
    if not isinstance(nodes, list):
        raise ValueError('"nodes" is not an array')
    if len(nodes) > MAX_NODES:
        raise ValueError(f"the file has {len(nodes)} nodes, over the limit of {MAX_NODES}")
    scenes = doc.get("scenes")
    if isinstance(scenes, list) and scenes:
        si = doc.get("scene", 0)
        if not isinstance(si, int) or isinstance(si, bool) or not 0 <= si < len(scenes) or not isinstance(scenes[si], dict):
            raise ValueError('"scene" does not name a scene')
        roots = scenes[si].get("nodes", [])
    else:  # no scenes: every node nobody lists as a child
        kids = set()
        for n in nodes:
            if isinstance(n, dict) and isinstance(n.get("children"), list):
                kids.update(c for c in n["children"] if isinstance(c, int))
        roots = [i for i in range(len(nodes)) if i not in kids]
    if not isinstance(roots, list):
        raise ValueError("a scene's \"nodes\" is not an array")
    out, seen = [], set()
    stack = [(r, IDENTITY, 1) for r in reversed(roots)]
    while stack:
        idx, parent, depth = stack.pop()
        if not isinstance(idx, int) or isinstance(idx, bool) or not 0 <= idx < len(nodes) or not isinstance(nodes[idx], dict):
            raise ValueError(f"node index {idx!r} is out of range")
        if idx in seen:
            raise ValueError(f"node {idx} is reached twice: a node has one parent and the tree has no cycles")
        if depth > MAX_DEPTH:
            raise ValueError(f"the node tree is deeper than the limit of {MAX_DEPTH}")
        seen.add(idx)
        world = mat_mul(parent, node_matrix(nodes[idx], idx))
        out.append((idx, world))
        kids = nodes[idx].get("children", [])
        if not isinstance(kids, list):
            raise ValueError(f"node {idx} children is not an array")
        for c in reversed(kids):
            stack.append((c, world, depth + 1))
    return out


def convert_gltf(doc, scale, imp):
    """[placed entity dicts] for the lights and cameras of the default scene."""
    ext = doc.get("extensions") or {}
    lights = ((ext.get("KHR_lights_punctual") or {}).get("lights")) if isinstance(ext, dict) else None
    if lights is not None and not isinstance(lights, list):
        raise ValueError("KHR_lights_punctual \"lights\" is not an array")
    cams = doc.get("cameras", [])
    if not isinstance(cams, list):
        raise ValueError('"cameras" is not an array')
    nodes = doc.get("nodes", [])
    placed, n_cam, n_light = [], 0, 0
    for idx, world in gltf_walk(doc):
        node = nodes[idx]
        who = f"'{clean_name(node.get('name'), '')}'" if clean_name(node.get("name"), "") else f"(node {idx})"
        lref = ((node.get("extensions") or {}).get("KHR_lights_punctual") or {}).get("light") \
            if isinstance(node.get("extensions"), dict) else None
        cref = node.get("camera")
        if lref is None and cref is None:
            continue
        pos, rot, mirrored = world_pose(world, scale, who)
        if mirrored:
            imp.warn(f"node {who} has a mirrored (negative scale) world transform; the mirroring is dropped")
        name = clean_name(node.get("name"), "")
        if lref is not None:
            if not lights or not isinstance(lref, int) or isinstance(lref, bool) or not 0 <= lref < len(lights) or \
                    not isinstance(lights[lref], dict):
                raise ValueError(f"node {idx} names light {lref!r}, which does not exist")
            lt = lights[lref]
            kind = lt.get("type")
            color = numbers(lt.get("color", [1, 1, 1]), 3, f"light {lref} color")
            if any(not 0 <= c <= 1 for c in color):
                imp.warn(f"light {who}: colour {vec_text(color)} is outside 0..1; clamped")
                color = [min(1.0, max(0.0, c)) for c in color]
            inten = finite(lt.get("intensity", 1.0), f"light {lref} intensity")
            if inten < 0:
                raise ValueError(f"light {lref} has a negative intensity")
            if kind == "directional":
                sun_warning(imp, who, rotate(rot, [0, 0, -1]), inten, True)
            else:  # (the node may also carry a camera: handled below)
                if kind not in ("point", "spot"):
                    raise ValueError(f"light {lref} has type {kind!r} (point, spot or directional)")
                engine = inten / (LUMENS_PER_WATT * math.pi)
                if engine > MAX_INTENSITY:
                    imp.warn(f"light {who}: {num(inten, 4):g} cd is {num(engine, 4):g} in engine units; clamped to {MAX_INTENSITY:g}")
                    engine = MAX_INTENSITY
                rng = lt.get("range")
                rng = finite(rng, f"light {lref} range") * scale if rng is not None else None
                if rng is not None and rng <= 0:
                    raise ValueError(f"light {lref} has a range of 0 or less")
                if rng is not None and not 0.1 <= rng <= 200.0:
                    imp.warn(f"light {who}: range {num(rng, 4):g} m is outside the engine's 0.1..200; clamped")
                radius = light_radius(engine, rng)
                n_light += 1
                if kind == "spot":
                    sp = lt.get("spot") or {}
                    if not isinstance(sp, dict):
                        raise ValueError(f"light {lref} spot is not an object")
                    outer = math.degrees(finite(sp.get("outerConeAngle", math.pi / 4), f"light {lref} outerConeAngle"))
                    inner = math.degrees(finite(sp.get("innerConeAngle", 0.0), f"light {lref} innerConeAngle"))
                    if not 1.0 <= outer <= 89.0:
                        clamped = min(89.0, max(1.0, outer))
                        imp.warn(f"light {who}: outer cone {num(outer, 4):g} deg is outside the engine's 1..89; clamped to {clamped:g}")
                        outer = clamped
                    inner = clamp_inner(imp, who, inner, outer)
                    comps = point_or_spot_components("spot", color, engine, radius, [0, 0, -1], outer, inner)
                    fallback = f"Spot Light {n_light}"
                else:
                    comps = point_or_spot_components("point", color, engine, radius)
                    fallback = f"Point Light {n_light}"
                placed.append({"name": name, "fallback": fallback, "pos": pos, "rot": rot, "components": comps})
        if cref is not None:
            if not isinstance(cref, int) or isinstance(cref, bool) or not 0 <= cref < len(cams) or not isinstance(cams[cref], dict):
                raise ValueError(f"node {idx} names camera {cref!r}, which does not exist")
            cam = cams[cref]
            if cam.get("type") != "perspective":
                imp.warn(f"camera {who} is {cam.get('type')!r}; the engine's cameras are perspective, so it is dropped")
                continue
            p = cam.get("perspective")
            if not isinstance(p, dict):
                raise ValueError(f"camera {cref} has no perspective object")
            yfov = finite(p.get("yfov"), f"camera {cref} yfov")
            znear = finite(p.get("znear"), f"camera {cref} znear") * scale
            zfar = finite(p["zfar"], f"camera {cref} zfar") * scale if p.get("zfar") is not None else 0.0
            if not 0 < yfov < math.pi:
                raise ValueError(f"camera {cref} yfov is outside (0, pi)")
            if zfar == 0.0:
                imp.warn(f"camera {who} has an infinite far plane; using the engine's default 1000 m")
                zfar = 1000.0
            n_cam += 1
            comps = camera_components(math.degrees(yfov), znear, zfar, n_cam == 1, imp, who)
            placed.append({"name": name, "fallback": f"Camera {n_cam}", "pos": pos, "rot": rot, "components": comps})
    return placed


# --- FBX -----------------------------------------------------------------------------------------------------------

def find_cook(explicit):
    cands = [explicit] if explicit else []
    env = os.environ.get("MCE_COOK")
    if env:
        cands.append(env)
    here = os.path.dirname(os.path.abspath(__file__))
    exe = "mce_cook.exe" if os.name == "nt" else "mce_cook"
    cands.append(os.path.join(here, exe))
    cands.append(os.path.join(here, "..", "..", "bin", exe))
    for c in cands:
        if c and os.path.isfile(c):
            return c
    raise ValueError("mce_cook was not found (FBX is read by the cook's own importer): pass --cook <path>, set MCE_COOK, "
                     "or run `mce import fbx` from an SDK install")


def dump_fbx(path, cook):
    size = os.path.getsize(path)
    if size > MAX_FBX_BYTES:
        raise ValueError(f"the file is {size >> 20} MiB, over the importer's limit of {MAX_FBX_BYTES >> 20} MiB")
    try:
        r = subprocess.run([cook, "--dump-fbx-scene", path], capture_output=True, timeout=300)
    except subprocess.TimeoutExpired:
        raise ValueError("mce_cook took over 5 minutes to read the FBX") from None
    except OSError as e:
        raise ValueError(f"cannot run {cook}: {e}") from None
    if r.returncode != 0:
        msg = r.stderr.decode("utf-8", "replace").strip().splitlines()
        raise ValueError(msg[-1] if msg else f"mce_cook exited with {r.returncode}")
    if len(r.stdout) > MAX_DUMP_BYTES:
        raise ValueError("the scene dump is over its size limit")
    try:
        doc = json.loads(r.stdout.decode("utf-8", "replace"))
    except (ValueError, RecursionError):
        raise ValueError("mce_cook printed a scene dump that is not JSON (is it an older mce_cook?)") from None
    if not isinstance(doc, dict) or doc.get("format") != "mce-fbx-scene":
        raise ValueError("mce_cook printed something that is not an FBX scene dump (is it an older mce_cook?)")
    return doc


def convert_fbx(doc, scale, imp):
    nodes = doc.get("nodes")
    if not isinstance(nodes, list) or len(nodes) > MAX_NODES:
        raise ValueError("the scene dump has no usable node list")
    placed, n_cam, n_light = [], 0, 0
    for i, n in enumerate(nodes):
        if not isinstance(n, dict):
            raise ValueError("the scene dump is malformed")
        name = clean_name(n.get("name"), "")
        who = f"'{name}'" if name else f"(node {i})"
        pos = [p * scale for p in numbers(n.get("translation"), 3, "a node translation")]
        scl = numbers(n["scale"], 3, "a node scale") if n.get("scale") is not None else [1.0, 1.0, 1.0]
        if min(abs(v) for v in scl) < 1e-12:
            raise ValueError(f"node {who} has a world transform with a zero scale")
        if scl[0] * scl[1] * scl[2] < 0:
            imp.warn(f"node {who} has a mirrored (negative scale) world transform; the mirroring is dropped")
        rot = numbers(n.get("rotation"), 4, "a node rotation")
        if all(v == 0 for v in rot):
            raise ValueError("a node rotation has zero length")
        norm = math.sqrt(sum(v * v for v in rot))
        rot = [v / norm for v in rot]
        if rot[3] < 0:
            rot = [-v for v in rot]
        lt = n.get("light")
        if isinstance(lt, dict):
            kind = lt.get("type")
            color = numbers(lt.get("color"), 3, "a light color")
            if any(not 0 <= c <= 1 for c in color):
                imp.warn(f"light {who}: colour {vec_text(color)} is outside 0..1; clamped")
                color = [min(1.0, max(0.0, c)) for c in color]
            inten = finite(lt.get("intensity"), "a light intensity")
            direction = numbers(lt.get("direction"), 3, "a light direction")
            on = bool(lt.get("enabled", 1))  # the file's "cast light" switch; the components have an `enabled`
            if kind == "directional":
                sun_warning(imp, who, rotate(rot, direction), inten, False, not on)
            elif kind in ("point", "spot"):
                if lt.get("decay") not in (None, "quadratic"):
                    imp.warn(f"light {who}: decay '{lt.get('decay')}' is ignored; engine lights always fall off with the inverse "
                             "square")
                engine = max(0.0, inten)
                if engine > MAX_INTENSITY:
                    imp.warn(f"light {who}: intensity {num(engine, 4):g} is over the engine's {MAX_INTENSITY:g}; clamped")
                    engine = MAX_INTENSITY
                radius = light_radius(engine, None)
                n_light += 1
                if kind == "spot":
                    dn = math.sqrt(sum(v * v for v in direction))
                    if dn == 0:
                        raise ValueError(f"light {who} has no aim direction")
                    direction = [v / dn for v in direction]
                    outer = finite(lt.get("outer_deg"), "a light outer angle") / 2  # FBX cone angles are full angles
                    inner = finite(lt.get("inner_deg"), "a light inner angle") / 2
                    if outer <= 0:
                        imp.warn(f"light {who} has no cone angle; using FBX's default of 45 deg (full angle), 22.5 deg here")
                        outer = 22.5
                    if not 1.0 <= outer <= 89.0:
                        clamped = min(89.0, max(1.0, outer))
                        imp.warn(f"light {who}: outer cone {num(outer, 4):g} deg is outside the engine's 1..89; clamped to {clamped:g}")
                        outer = clamped
                    inner = clamp_inner(imp, who, inner, outer)
                    comps = point_or_spot_components("spot", color, engine, radius, direction, outer, inner, on)
                    fallback = f"Spot Light {n_light}"
                else:
                    comps = point_or_spot_components("point", color, engine, radius, enabled=on)
                    fallback = f"Point Light {n_light}"
                placed.append({"name": name, "fallback": fallback, "pos": pos, "rot": rot, "components": comps})
            else:
                what = {"area": "an area light", "volume": "a volume light"}.get(
                    kind, f"a light of an unrecognised type (ufbx type {lt.get('type_id', '?')})")
                imp.warn(f"light {who} is {what}, which the engine has no equivalent for; dropped")
        cm = n.get("camera")
        if isinstance(cm, dict):
            if not cm.get("perspective", 1):
                imp.warn(f"camera {who} is orthographic; the engine's cameras are perspective, so it is dropped")
                continue
            n_cam += 1
            comps = camera_components(finite(cm.get("fov_y_deg"), "a camera field of view"),
                                      finite(cm.get("near"), "a camera near plane") * scale,
                                      finite(cm.get("far"), "a camera far plane") * scale, n_cam == 1, imp, who)
            placed.append({"name": name, "fallback": f"Camera {n_cam}", "pos": pos, "rot": rot, "components": comps})
    return placed


# --- the command ---------------------------------------------------------------------------------------------------

def read_json(path):
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8-sig"))


def existing_guid(path):
    """The GUID of a prefab written earlier, None when there is none or it is unreadable (it is rewritten then)."""
    try:
        g = read_json(path).get("guid")
        return g if isinstance(g, str) and g else None
    except (OSError, ValueError, AttributeError, RecursionError):
        return None


def read_sidecar(side):
    """(guid, scale) of an existing model sidecar, (None, 1.0) when there is none. A sidecar that cannot be read or has no
    "guid" is an error: replacing it would give the model a new identity and break every reference to it. The "scale"
    setting is what the cook multiplies every root node by; 1 when absent."""
    if not os.path.exists(side):
        return None, 1.0
    try:
        doc = read_json(side)
    except (OSError, ValueError, RecursionError) as e:
        why = (str(e).splitlines() or [type(e).__name__])[0]
        raise ValueError(f"{side} exists but cannot be read as JSON ({why}); fix or delete it") from None
    guid = doc.get("guid") if isinstance(doc, dict) else None
    if not isinstance(guid, str) or not guid:
        raise ValueError(f'{side} exists but has no "guid"; fix or delete it')
    settings = doc.get("settings")
    s = settings.get("scale", 1.0) if isinstance(settings, dict) else 1.0
    ok = isinstance(s, (int, float)) and not isinstance(s, bool) and math.isfinite(s) and 0 < s < 1e6
    return guid, float(s) if ok else 1.0


def safe_name(s):
    out = "".join(c if c.isalnum() or c in "-_" else "_" for c in str(s)).strip("_")
    return out or "model"


def run(tool, args):
    imp = Importer(tool)
    src = args.file
    try:
        if not os.path.isfile(src):
            raise ValueError("no such file")
        ext = os.path.splitext(src)[1].lower()
        if tool == "gltf" and ext not in (".gltf", ".glb"):
            raise ValueError("expected a .gltf or .glb file")
        if tool == "fbx" and ext != ".fbx":
            raise ValueError("expected a .fbx file")
        side = src + ".mceasset"
        model_guid, scale = read_sidecar(side)
        if tool == "gltf":
            placed = convert_gltf(read_gltf_json(src), scale, imp)
        else:
            placed = convert_fbx(dump_fbx(src, find_cook(args.cook)), scale, imp)
        out_dir = args.out_dir or os.path.dirname(os.path.abspath(src))
        name = safe_name(args.name or os.path.splitext(os.path.basename(src))[0])
        out = os.path.join(out_dir, name + ".mcprefab")
        new_side = model_guid is None
        if new_side:
            model_guid = str(uuid.uuid4())
        guid = existing_guid(out) or str(uuid.uuid4())
        prefab = build_prefab(name, guid, "guid:" + model_guid, placed)
    except (OSError, ValueError, KeyError, TypeError, AttributeError, RecursionError, OverflowError) as e:
        print(f"mce import {tool}: {src}: {e}", file=sys.stderr)
        return 1
    if imp.warnings and args.werror:
        print(f"mce import {tool}: --werror: {len(imp.warnings)} warning(s); nothing written", file=sys.stderr)
        return 1
    os.makedirs(out_dir, exist_ok=True)
    if new_side:
        with open(side, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"guid": model_guid, "importer": tool,
                       "importer_version": GLTF_IMPORTER_VERSION if tool == "gltf" else FBX_IMPORTER_VERSION,
                       "settings": {}}, f, indent=2)
            f.write("\n")
        print(f"wrote {side}")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(prefab, indent=2) + "\n")
    lights = sum(1 for p in placed if "Camera3DComponent" not in p["components"])
    print(f"wrote {out} ({lights} light(s), {len(placed) - lights} camera(s))")
    return 0
