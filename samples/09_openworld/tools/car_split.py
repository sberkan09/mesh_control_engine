"""Splits a Kenney Car Kit .glb into a body model and one wheel model (M-WHEELSPIN 57c).

The importer merges all nodes of a model into one mesh, so a car cannot spin its wheels. This reads the original glb (its
nodes are separate: `body`, `wheel-front-left`, ...), and writes two compact glbs: the body with every wheel node left out
(the SUV's spare `wheel-back` on the tailgate stays), and the front-left wheel centred on its own origin so a hub entity can
spin it about X. Deterministic: same input bytes, same output bytes.
"""
import json
import struct

WHEEL_NODES = ("wheel-front-left", "wheel-front-right", "wheel-back-left", "wheel-back-right")


def read_glb(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[12:16])[0]
    j = json.loads(d[20:20 + n])
    off = 20 + n
    bn = struct.unpack("<I", d[off:off + 4])[0]
    return j, d[off + 8:off + 8 + bn]


def _build(j, bin_, node_ids, tex_prefix, translate=None):
    """A glb holding only `node_ids` of `j` (compact buffer, textures resolved through `tex_prefix`)."""
    nodes, meshes, accessors, views, blob = [], [], [], [], bytearray()
    acc_map, mesh_map = {}, {}

    def accessor(i):
        if i not in acc_map:
            a = dict(j["accessors"][i])
            v = j["bufferViews"][a["bufferView"]]
            data = bin_[v.get("byteOffset", 0):v.get("byteOffset", 0) + v["byteLength"]]
            while len(blob) % 4:
                blob.append(0)
            nv = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
            for k in ("byteStride", "target"):
                if k in v:
                    nv[k] = v[k]
            blob.extend(data)
            views.append(nv)
            a["bufferView"] = len(views) - 1
            acc_map[i] = len(accessors)
            accessors.append(a)  # its own byteOffset (if any) stays valid: the whole original view is copied
        return acc_map[i]

    order = []  # the chosen nodes and their descendants, in file order
    def add(ni):
        if ni not in order:
            order.append(ni)
            for c in j["nodes"][ni].get("children", []):
                add(c)
    for ni in node_ids:
        add(ni)
    new_index = {ni: k for k, ni in enumerate(order)}
    for ni in order:
        n = dict(j["nodes"][ni])
        if "children" in n:
            n["children"] = [new_index[c] for c in n["children"]]
        mi = n.get("mesh")
        if mi is None:
            nodes.append(n)
            continue
        if mi not in mesh_map:
            m = json.loads(json.dumps(j["meshes"][mi]))
            for p in m["primitives"]:
                p["attributes"] = {k: accessor(v) for k, v in p["attributes"].items()}
                if "indices" in p:
                    p["indices"] = accessor(p["indices"])
            mesh_map[mi] = len(meshes)
            meshes.append(m)
        n["mesh"] = mesh_map[mi]
        if translate is not None:
            n["translation"] = list(translate)
        nodes.append(n)
    while len(blob) % 4:
        blob.append(0)
    out = {"asset": {"version": "2.0", "generator": "mce car_split"}, "scene": 0,
           "scenes": [{"nodes": [new_index[i] for i in node_ids]}], "nodes": nodes, "meshes": meshes, "accessors": accessors,
           "bufferViews": views, "buffers": [{"byteLength": len(blob)}], "materials": j["materials"]}
    for k in ("textures", "samplers", "extensionsUsed", "extensionsRequired"):
        if k in j:
            out[k] = j[k]
    if "images" in j:
        out["images"] = [dict(im, uri=tex_prefix + im["uri"]) for im in j["images"]]
    js = json.dumps(out, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    return (struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(js), 0x4E4F534A) + js
            + struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


def split(path, tex_prefix):
    """-> (body_glb, wheel_glb, info). `info` has the wheel centres in model space: front/back (x, y, z) of the left wheels
    (model +x), and the wheel's radius."""
    j, bin_ = read_glb(path)
    names = [n["name"] for n in j["nodes"]]
    scene = j["scenes"][j.get("scene", 0)]["nodes"]
    body = [i for i in scene if names[i] not in WHEEL_NODES]
    fl = j["nodes"][names.index("wheel-front-left")]
    bl = j["nodes"][names.index("wheel-back-left")]
    acc = j["accessors"][j["meshes"][fl["mesh"]]["primitives"][0]["attributes"]["POSITION"]]
    cx = (acc["min"][0] + acc["max"][0]) / 2
    radius = (acc["max"][1] - acc["min"][1]) / 2
    wheel = _build(j, bin_, [names.index("wheel-front-left")], tex_prefix, translate=(-cx, 0.0, 0.0))
    info = {"front": [round(c, 4) for c in fl["translation"]], "back": [round(c, 4) for c in bl["translation"]],
            "cx": round(cx, 4), "radius": round(radius, 4), "width": round(acc["max"][0] - acc["min"][0], 4)}
    return _build(j, bin_, body, tex_prefix), wheel, info
