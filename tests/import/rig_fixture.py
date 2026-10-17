"""Small skinned humanoid .glb files built in code, for the retarget and content tests (no binary fixtures).

A rig is a list of (name, parent name or None, local translation, local rotation); every bone is a joint of one skin,
and a one-triangle mesh is weighted to the first bone so the importer has something to cook.
"""

import json
import math
import struct


def quat_z(deg):
    a = math.radians(deg) / 2
    return (0.0, 0.0, math.sin(a), math.cos(a))


def quat_y(deg):
    a = math.radians(deg) / 2
    return (0.0, math.sin(a), 0.0, math.cos(a))


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _rot(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))


def humanoid(names, offsets, root_rot=(0.0, 0.0, 0.0, 1.0)):
    """A rig from a name table and {role: local offset}. `names` maps role -> bone name; the roles are the
    engine-style ones below. Left bones sit at +x with offsets given for the left side; the right side mirrors x."""
    tree = [("hips", None), ("spine", "hips"), ("chest", "spine"), ("neck", "chest"), ("head", "neck"),
            ("shoulder_l", "chest"), ("upper_arm_l", "shoulder_l"), ("forearm_l", "upper_arm_l"), ("hand_l", "forearm_l"),
            ("shoulder_r", "chest"), ("upper_arm_r", "shoulder_r"), ("forearm_r", "upper_arm_r"), ("hand_r", "forearm_r"),
            ("thigh_l", "hips"), ("shin_l", "thigh_l"), ("foot_l", "shin_l"), ("toes_l", "foot_l"),
            ("thigh_r", "hips"), ("shin_r", "thigh_r"), ("foot_r", "shin_r"), ("toes_r", "foot_r")]
    rig = []
    for role, parent in tree:
        if role not in names:
            continue
        off = offsets[role if role in offsets else role.rsplit("_", 1)[0] + "_l"]
        if role.endswith("_r"):
            off = (-off[0], off[1], off[2])
        rig.append((names[role], names[parent] if parent else None, off, root_rot if parent is None else (0.0, 0.0, 0.0, 1.0)))
    return rig


def world_matrices(rig):
    """{bone name: (position, rotation)} at rest."""
    out = {}
    for name, parent, t, r in rig:
        if parent is None:
            out[name] = (t, r)
        else:
            pp, pr = out[parent]
            q = _rot(pr, t)
            out[name] = ((pp[0] + q[0], pp[1] + q[1], pp[2] + q[2]), _qmul(pr, r))
    return out


def _inverse_bind(pos, rot):
    # inverse of T(pos) * R(rot), column-major: R^T, -R^T p
    x, y, z, w = rot
    r = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]  # column-major R: r[col*3+row]
    # transpose: element (row i, col j) of R^T is R(j, i)
    m = [r[0], r[3], r[6], 0, r[1], r[4], r[7], 0, r[2], r[5], r[8], 0]
    tx = -(m[0] * pos[0] + m[4] * pos[1] + m[8] * pos[2])
    ty = -(m[1] * pos[0] + m[5] * pos[1] + m[9] * pos[2])
    tz = -(m[2] * pos[0] + m[6] * pos[1] + m[10] * pos[2])
    return m[:12] + [tx, ty, tz, 1]


def build_glb(rig, clips=None, mesh=True, armature=True):
    """Bytes of a .glb. clips: {name: {"rot": {bone: [(time, quat)]}, "trans": {bone: [(time, vec)]}}}."""
    index = {}
    nodes = []
    if armature:
        nodes.append({"name": "Armature", "children": []})
    for name, parent, t, r in rig:
        index[name] = len(nodes)
        nodes.append({"name": name, "translation": list(t), "rotation": list(r)})
    for name, parent, t, r in rig:
        if parent is None:
            if armature:
                nodes[0]["children"].append(index[name])
        else:
            nodes[index[parent]].setdefault("children", []).append(index[name])
    scene_nodes = [0] if armature else [index[n] for n, p, t, r in rig if p is None]
    blob = bytearray()
    views, accessors = [], []

    def add(data_bytes, comp, typ, count, mm=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data_bytes)})
        blob.extend(data_bytes)
        a = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": typ}
        if mm:
            a["min"], a["max"] = mm
        accessors.append(a)
        return len(accessors) - 1

    j = {"asset": {"version": "2.0", "generator": "mce test rig"}}
    if mesh:
        world = world_matrices(rig)
        hips = rig[0][0]
        p0 = world[hips][0]
        pos = [(p0[0] - 0.1, p0[1], p0[2]), (p0[0] + 0.1, p0[1], p0[2]), (p0[0], p0[1] + 0.2, p0[2])]
        a_pos = add(struct.pack("<9f", *[c for p in pos for c in p]), 5126, "VEC3", 3,
                    ([min(p[i] for p in pos) for i in range(3)], [max(p[i] for p in pos) for i in range(3)]))
        a_nrm = add(struct.pack("<9f", 0, 0, 1, 0, 0, 1, 0, 0, 1), 5126, "VEC3", 3)
        a_jnt = add(bytes([0, 0, 0, 0] * 3), 5121, "VEC4", 3)
        a_wgt = add(struct.pack("<12f", *([1, 0, 0, 0] * 3)), 5126, "VEC4", 3)
        a_idx = add(struct.pack("<3H", 0, 1, 2), 5123, "SCALAR", 3)
        ibm = []
        for name, parent, t, r in rig:
            ibm += _inverse_bind(*world[name])
        a_ibm = add(struct.pack(f"<{len(ibm)}f", *ibm), 5126, "MAT4", len(rig))
        nodes.append({"name": "body", "mesh": 0, "skin": 0})
        scene_nodes.append(len(nodes) - 1)
        j["meshes"] = [{"name": "body", "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "JOINTS_0": a_jnt,
                                                                    "WEIGHTS_0": a_wgt}, "indices": a_idx}]}]
        j["skins"] = [{"name": "skin", "joints": [index[n] for n, p, t, r in rig], "inverseBindMatrices": a_ibm,
                       "skeleton": index[rig[0][0]]}]
    anims = []
    for cname, clip in (clips or {}).items():
        samplers, channels = [], []
        for path, key in (("rotation", "rot"), ("translation", "trans")):
            for bone, keys in clip.get(key, {}).items():
                times = [k[0] for k in keys]
                vals = [c for k in keys for c in k[1]]
                ti = add(struct.pack(f"<{len(times)}f", *times), 5126, "SCALAR", len(times), ([min(times)], [max(times)]))
                vi = add(struct.pack(f"<{len(vals)}f", *vals), 5126, "VEC4" if path == "rotation" else "VEC3", len(times))
                samplers.append({"input": ti, "interpolation": "LINEAR", "output": vi})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": index[bone], "path": path}})
        anims.append({"name": cname, "samplers": samplers, "channels": channels})
    if anims:
        j["animations"] = anims
    j["nodes"] = nodes
    j["scenes"] = [{"nodes": scene_nodes}]
    j["scene"] = 0
    j["bufferViews"] = views
    j["accessors"] = accessors
    j["buffers"] = [{"byteLength": len(blob)}]
    js = json.dumps(j, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    bn = bytes(blob) + b"\0" * (-len(blob) % 4)
    total = 12 + 8 + len(js) + 8 + len(bn)
    return struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(js), 0x4E4F534A) + js + \
        struct.pack("<II", len(bn), 0x004E4942) + bn


def fk(rig, pose):
    """{bone: (world position, world rotation)} with `pose` {bone: {"r": quat, "t": vec}} overriding the rest locals."""
    out = {}
    for name, parent, t, r in rig:
        p = pose.get(name, {})
        t, r = p.get("t", t), p.get("r", r)
        if parent is None:
            out[name] = (t, r)
        else:
            pp, pr = out[parent]
            q = _rot(pr, t)
            out[name] = ((pp[0] + q[0], pp[1] + q[1], pp[2] + q[2]), _qmul(pr, r))
    return out


qmul = _qmul
rotate = _rot


def qconj_(q):
    return (-q[0], -q[1], -q[2], q[3])
