#!/usr/bin/env python3
"""mce retarget: animation clips from one humanoid glTF onto another humanoid's skeleton (M-OPENWORLD D3).

    mce retarget <source.glb|.gltf>... --onto <character.glb> [--out f.glb | --inplace]
                 [--clips a,b] [--prefix p] [--map f.json] [--replace] [--drop-existing]
                 [--in-place-motion] [--no-translation] [--scale auto|N] [--map-only] [--strict]

The engine plays a clip on the nodes of the file it was cooked with (the glTF importer puts a mesh's clips in its
CHNK_MESH), so a character gets motion from an animation library by copying the library's clips into the
character's own .glb: this tool reads every clip of each <source>, retargets it onto <character.glb>'s skeleton
and writes a .glb that is the character plus those clips. The mesh, skin, materials and external texture URIs of
the character are copied unchanged; cook it like any model.

How: both skeletons are matched bone by bone through a humanoid map (hips, spine chain, neck, head, shoulders,
arms, hands, thighs, shins, feet, toes and the fingers), found from the bone names (Mixamo, Unreal, Rigify, VRM,
Biped, Kenney/Quaternius-style and the engine's own `hips`/`upper_arm_l`/... names), or given with --map. The
motion is carried in world space: for each bone, the rotation the source bone made away from its rest pose is
applied to the target bone's rest pose, so an A-pose library drives a T-pose character (and the other way round).
A source that faces another way is turned to match the target. The hips' translation is scaled by the ratio of
the two hips' heights above the feet; --scale N forces a ratio, --in-place-motion drops the horizontal travel
(for a locomotion cycle the game moves by code: the straight line from the first key's
position to the last is removed, the sway stays), --no-translation keeps the hips where they rest.

Bones one side lacks stay at their rest pose or follow their parent: a target with one spine bone takes the top of a
source's spine chain, a source without a chest drives a target's chest from the spine. Source bones with no
target (twist, helper, IK-target bones) are listed in a warning. Scale and morph-weight channels are not carried.
A pair the names do not find is given in --map as {"SourceBoneName": "TargetBoneName"}.

Warnings go to stderr and the exit code stays 0; --strict makes any warning a failure (nothing is written).
The output is a pure function of its inputs.
"""

import argparse
import base64
import bisect
import json
import math
import os
import re
import struct
import sys
import urllib.parse
import uuid

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
TYPE_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}


class RetargetError(Exception):
    pass


# ---- glTF container ---------------------------------------------------------------------------------------

class Gltf:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, "rb") as f:
            raw = f.read()
        self.glb = raw[:4] == b"glTF"
        if self.glb:
            if len(raw) < 20:
                raise RetargetError(f"{self.name}: truncated GLB")
            magic, version, length = struct.unpack_from("<III", raw, 0)
            if magic != GLB_MAGIC or version != 2:
                raise RetargetError(f"{self.name}: not a glTF 2.0 binary")
            pos = 12
            self.json = None
            bin0 = b""
            while pos + 8 <= min(length, len(raw)):
                clen, ctype = struct.unpack_from("<II", raw, pos)
                body = raw[pos + 8:pos + 8 + clen]
                if ctype == CHUNK_JSON and self.json is None:
                    self.json = json.loads(body.decode("utf-8"))
                elif ctype == CHUNK_BIN and not bin0:
                    bin0 = bytes(body)
                pos += 8 + ((clen + 3) & ~3)
            if self.json is None:
                raise RetargetError(f"{self.name}: no JSON chunk")
            self.glb_bin = bin0
        else:
            try:
                self.json = json.loads(raw.decode("utf-8-sig"))
            except ValueError as e:
                raise RetargetError(f"{self.name}: not glTF JSON: {e}")
            self.glb_bin = b""
        self.buffers = []
        for i, b in enumerate(self.json.get("buffers", [])):
            uri = b.get("uri")
            if uri is None:
                if i != 0 or not self.glb:
                    raise RetargetError(f"{self.name}: buffer {i} has no data")
                self.buffers.append(self.glb_bin)
            elif uri.startswith("data:"):
                self.buffers.append(base64.b64decode(uri.split(",", 1)[1]))
            else:
                p = os.path.join(os.path.dirname(os.path.abspath(path)), urllib.parse.unquote(uri))
                try:
                    with open(p, "rb") as f:
                        self.buffers.append(f.read())
                except OSError as e:
                    raise RetargetError(f"{self.name}: buffer {uri}: {e}")

    def accessor(self, index):
        """The accessor's elements as a list of floats (SCALAR) or tuples."""
        a = self.json["accessors"][index]
        if "sparse" in a:
            raise RetargetError(f"{self.name}: sparse accessor {index} is not supported (animation data should not use it)")
        fmt, size = COMPONENT[a["componentType"]]
        n = TYPE_COUNT[a["type"]]
        norm = a.get("normalized", False)
        count = a["count"]
        if "bufferView" not in a:
            return [0.0 if n == 1 else (0.0,) * n for _ in range(count)]
        bv = self.json["bufferViews"][a["bufferView"]]
        buf = self.buffers[bv.get("buffer", 0)]
        stride = bv.get("byteStride") or size * n
        base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        out = []
        for i in range(count):
            vals = struct.unpack_from("<" + fmt * n, buf, base + i * stride)
            if norm and fmt != "f":
                div = {"b": 127.0, "B": 255.0, "h": 32767.0, "H": 65535.0}.get(fmt, 1.0)
                vals = tuple(max(v / div, -1.0) if fmt in "bh" else v / div for v in vals)
            out.append(float(vals[0]) if n == 1 else tuple(float(v) for v in vals))
        return out


# ---- quaternion and matrix helpers ------------------------------------------------------------------------

def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q))
    return (0.0, 0.0, 0.0, 1.0) if n < 1e-12 else tuple(c / n for c in q)


def qdot(a, b):
    return sum(x * y for x, y in zip(a, b))


def qslerp(a, b, t):
    d = qdot(a, b)
    if d < 0:
        b = tuple(-c for c in b)
        d = -d
    if d > 0.9995:
        return qnorm(tuple(x + (y - x) * t for x, y in zip(a, b)))
    th = math.acos(min(1.0, d))
    s = math.sin(th)
    wa, wb = math.sin((1 - t) * th) / s, math.sin(t * th) / s
    return qnorm(tuple(wa * x + wb * y for x, y in zip(a, b)))


def qrot(q, v):
    x, y, z, w = q
    # v' = v + 2w (q x v) + 2 q x (q x v)
    tx = 2 * (y * v[2] - z * v[1])
    ty = 2 * (z * v[0] - x * v[2])
    tz = 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty),
            v[1] + w * ty + (z * tx - x * tz),
            v[2] + w * tz + (x * ty - y * tx))


def qaxis_y(angle):
    return (0.0, math.sin(angle / 2), 0.0, math.cos(angle / 2))


def vadd(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def vsub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def vscale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def mat_to_trs(m):
    """Column-major 4x4 (glTF `matrix`) to (translation, rotation, scale); shear is dropped."""
    t = (m[12], m[13], m[14])
    sx = math.sqrt(m[0] ** 2 + m[1] ** 2 + m[2] ** 2)
    sy = math.sqrt(m[4] ** 2 + m[5] ** 2 + m[6] ** 2)
    sz = math.sqrt(m[8] ** 2 + m[9] ** 2 + m[10] ** 2)
    sx = sx or 1.0
    sy = sy or 1.0
    sz = sz or 1.0
    r = [m[0] / sx, m[1] / sx, m[2] / sx, m[4] / sy, m[5] / sy, m[6] / sy, m[8] / sz, m[9] / sz, m[10] / sz]
    m00, m10, m20, m01, m11, m21, m02, m12, m22 = r  # column-major: r[col*3+row]
    tr = m00 + m11 + m22
    if tr > 0:
        s = math.sqrt(tr + 1) * 2
        q = ((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s)
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1 + m00 - m11 - m22) * 2
        q = (0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s)
    elif m11 > m22:
        s = math.sqrt(1 + m11 - m00 - m22) * 2
        q = ((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s)
    else:
        s = math.sqrt(1 + m22 - m00 - m11) * 2
        q = ((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s)
    return t, qnorm(q), (sx, sy, sz)


# ---- skeletons --------------------------------------------------------------------------------------------

class Skeleton:
    """A glTF's nodes with parents and rest transforms, plus its humanoid bone map."""

    def __init__(self, gltf):
        self.gltf = gltf
        nodes = gltf.json.get("nodes", [])
        self.count = len(nodes)
        self.parent = [-1] * self.count
        for i, n in enumerate(nodes):
            for c in n.get("children", []):
                if 0 <= c < self.count:
                    self.parent[c] = i
        self.name = [n.get("name", f"node{i}") for i, n in enumerate(nodes)]
        self.rest_t, self.rest_r, self.rest_s = [], [], []
        for n in nodes:
            if "matrix" in n:
                t, r, s = mat_to_trs(n["matrix"])
            else:
                t = tuple(n.get("translation", (0, 0, 0)))
                r = qnorm(tuple(n.get("rotation", (0, 0, 0, 1))))
                s = tuple(n.get("scale", (1, 1, 1)))
            self.rest_t.append(t)
            self.rest_r.append(r)
            self.rest_s.append(s)
        self.order = self._topological()
        self.rest_world = self.world_pos_rot({})
        joints = set()
        for sk in gltf.json.get("skins", []):
            joints.update(sk.get("joints", []))
        self.joints = joints
        self.bones = {}  # canonical name -> node index

    def _topological(self):
        order, seen = [], [False] * self.count

        def visit(i):
            if seen[i]:
                return
            seen[i] = True
            if self.parent[i] >= 0:
                visit(self.parent[i])
            order.append(i)
        for i in range(self.count):
            visit(i)
        return order

    def depth(self, i):
        d = 0
        while self.parent[i] >= 0:
            i = self.parent[i]
            d += 1
        return d

    def world_pos_rot(self, pose):
        """([world position], [world rotation]) of every node; `pose` maps node -> {"t": v, "r": q} overrides."""
        pos = [None] * self.count
        rot = [None] * self.count
        scl = [1.0] * self.count
        for i in self.order:
            p = pose.get(i, {})
            t = p.get("t", self.rest_t[i])
            r = p.get("r", self.rest_r[i])
            s = (self.rest_s[i][0] + self.rest_s[i][1] + self.rest_s[i][2]) / 3.0
            par = self.parent[i]
            if par < 0:
                pos[i], rot[i], scl[i] = t, r, s
            else:
                pos[i] = vadd(pos[par], qrot(rot[par], vscale(t, scl[par])))
                rot[i] = qmul(rot[par], r)
                scl[i] = scl[par] * s
        self._scale = scl
        return pos, rot

    def world_scale(self, i):
        return self._scale[i]


# ---- humanoid bone matching -------------------------------------------------------------------------------

SIDE_TOKENS = {"left": "l", "l": "l", "lt": "l", "right": "r", "r": "r", "rt": "r"}
DROP_TOKENS = {"def", "mch", "org", "tweak", "bip01", "bip", "bone", "jbip", "j", "c", "sk", "skin", "jnt", "joint", "rig"}
FINGERS = ("thumb", "index", "middle", "ring", "pinky", "little")

SINGLE = {
    "hips": {"hips", "hip", "pelvis", "root_hips"},
    "neck": {"neck", "neck1", "neck01"},
    "head": {"head"},
}
SIDED = {
    "shoulder": {"shoulder", "clavicle", "collar", "collarbone"},
    "upper_arm": {"upperarm", "arm"},
    "forearm": {"forearm", "lowerarm", "lowarm"},
    "hand": {"hand", "wrist"},
    "thigh": {"thigh", "upleg", "upperleg", "uplegl"},
    "shin": {"shin", "calf", "leg", "lowerleg", "lowleg"},
    "foot": {"foot", "ankle"},
    "toes": {"toe", "toes", "toebase", "ball"},
}
SPINE_KEYS = re.compile(r"^(spine\d*|chest|upperchest|torso|abdomen|waist|trunk)$")


class _NoBones:
    """A skeleton seen without its humanoid map: only --map pairs count."""

    def __init__(self, sk):
        self.name, self.bones, self.spine_chain = sk.name, {}, []


def split_name(name):
    """(side or None, normalized key) of a bone name."""
    name = re.split(r"[:|]", name)[-1]
    name = re.sub(r"^mixamorig\d*[_:]?", "", name, flags=re.I)
    spaced = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", name)
    tokens = [t for t in re.split(r"[^A-Za-z0-9]+", spaced.lower()) if t]
    side = None
    kept = []
    for t in tokens:
        if t in SIDE_TOKENS:
            side = side or SIDE_TOKENS[t]
        elif t not in DROP_TOKENS:
            kept.append(t)
    return side, "".join(kept)


def finger_of(key):
    m = re.search(r"(thumb|index|middle|ring|pinky|pinkie|little)0*([1-4])?$", key)
    if not m:
        return None
    f = "pinky" if m.group(1) in ("pinkie", "little") else m.group(1)
    return f, int(m.group(2) or 1)


def match_humanoid(sk):
    """Fills sk.bones with canonical name -> node. Candidates are the skin's joints when there is a skin."""
    cand = [i for i in range(sk.count) if not sk.joints or i in sk.joints]
    found = {}

    def put(canon, node):
        # shallowest wins, then the lowest index
        old = found.get(canon)
        if old is None or (sk.depth(node), node) < (sk.depth(old), old):
            found[canon] = node

    spine = []
    for i in cand:
        side, key = split_name(sk.name[i])
        if side is None:
            for canon, keys in SINGLE.items():
                if key in keys:
                    put(canon, i)
            if SPINE_KEYS.match(key):
                spine.append(i)
        fk = finger_of(key)
        if side and fk:
            put(f"{fk[0]}_{fk[1]}_{side}", i)
            continue
        if side:
            for canon, keys in SIDED.items():
                if key in keys:
                    put(f"{canon}_{side}", i)
    # a skeleton with both a pelvis and a hips bone: the shallowest got "hips" above
    spine.sort(key=lambda n: (sk.depth(n), n))
    hips = found.get("hips")
    spine = [n for n in spine if n != hips]
    for j, n in enumerate(spine_roles(spine)):
        if n is not None:
            found[("spine", "chest", "upper_chest")[j]] = n
    sk.spine_chain = spine
    sk.bones = found


def spine_roles(chain):
    """(spine, chest, upper_chest) nodes from a hips-to-neck spine chain of any length."""
    if not chain:
        return (None, None, None)
    if len(chain) == 1:
        return (chain[0], None, None)
    if len(chain) == 2:
        return (chain[0], chain[1], None)
    return (chain[0], chain[len(chain) // 2], chain[-1])


def build_pairs(src, dst, explicit, auto=True):
    """[(source node, target node)] for the humanoid bones both have (when `auto`), plus the explicit pairs."""
    pairs = {}
    if not auto:
        src, dst = _NoBones(src), _NoBones(dst)
    for canon, dn in dst.bones.items():
        if canon in ("spine", "chest", "upper_chest"):
            continue
        sn = src.bones.get(canon)
        if sn is not None:
            pairs[dn] = sn
    # spine chain: resample the source chain over the target's
    sc, dc = getattr(src, "spine_chain", []), getattr(dst, "spine_chain", [])
    if sc and dc:
        n, m = len(sc), len(dc)
        for j, dn in enumerate(dc):
            k = min(n - 1, max(0, int(math.ceil((j + 1) * n / m)) - 1)) if n >= m else min(n - 1, int(j * n / m))
            pairs[dn] = sc[k]
    for s_name, d_name in explicit.items():
        sn = next((i for i, n in enumerate(src.name) if n == s_name), None)
        dn = next((i for i, n in enumerate(dst.name) if n == d_name), None)
        if sn is None or dn is None:
            raise RetargetError(f"--map: no bone '{s_name if sn is None else d_name}' in the "
                                f"{'source' if sn is None else 'target'}")
        pairs[dn] = sn
    return sorted(pairs.items())


# ---- clips ------------------------------------------------------------------------------------------------

class Channel:
    def __init__(self, times, values, interp):
        self.times, self.values, self.interp = times, values, interp

    def sample(self, t, quat):
        ts = self.times
        if len(ts) == 1 or t <= ts[0]:
            return self._val(0)
        if t >= ts[-1]:
            return self._val(len(ts) - 1)
        i = bisect.bisect_right(ts, t) - 1
        a = self._val(i)
        if self.interp == "STEP":
            return a
        b = self._val(i + 1)
        f = (t - ts[i]) / (ts[i + 1] - ts[i])
        if quat:
            return qslerp(a, b, f)
        return tuple(x + (y - x) * f for x, y in zip(a, b))

    def _val(self, i):
        if self.interp == "CUBICSPLINE":  # [in-tangent, value, out-tangent] per key: the value, tangents dropped
            return self.values[i * 3 + 1]
        return self.values[i]


def read_clip(gltf, anim, warn):
    rot, trans = {}, {}
    ignored = 0
    for ch in anim.get("channels", []):
        target = ch.get("target", {})
        node, path = target.get("node"), target.get("path")
        if node is None:
            continue
        smp = anim["samplers"][ch["sampler"]]
        times = gltf.accessor(smp["input"])
        values = gltf.accessor(smp["output"])
        interp = smp.get("interpolation", "LINEAR")
        if not times:
            continue
        if interp == "CUBICSPLINE":
            warn(f"clip '{anim.get('name', '?')}': CUBICSPLINE keys of {gltf.json['nodes'][node].get('name', node)} "
                 "are read as their values (tangents dropped)")
            if len(values) != 3 * len(times):
                raise RetargetError(f"{gltf.name}: a CUBICSPLINE sampler's output must hold 3 values per key")
        elif len(values) != len(times):
            raise RetargetError(f"{gltf.name}: a sampler's input and output counts differ")
        if path == "rotation":
            rot[node] = Channel(times, [qnorm(v) for v in values], interp)
        elif path == "translation":
            trans[node] = Channel(times, values, interp)
        else:
            ignored += 1
    return rot, trans, ignored


def face_yaw(sk):
    """Yaw (radians about +Y) of the skeleton's left-minus-right axis, or None."""
    pos = sk.rest_world[0]
    for a, b in (("thigh_l", "thigh_r"), ("upper_arm_l", "upper_arm_r"), ("shoulder_l", "shoulder_r"), ("foot_l", "foot_r")):
        if a in sk.bones and b in sk.bones:
            d = vsub(pos[sk.bones[a]], pos[sk.bones[b]])
            if abs(d[0]) + abs(d[2]) > 1e-9:
                return math.atan2(-d[2], d[0])
    return None


def hips_height(sk):
    pos = sk.rest_world[0]
    hips = sk.bones.get("hips")
    if hips is None:
        return None
    feet = [pos[sk.bones[k]][1] for k in ("foot_l", "foot_r", "toes_l", "toes_r") if k in sk.bones]
    floor = min(feet) if feet else 0.0
    h = pos[hips][1] - floor
    return h if h > 1e-6 else None


def wrap_angle(a):
    return a - 2 * math.pi * math.floor((a + math.pi) / (2 * math.pi))


def retarget_clip(src, dst, pairs, rot, trans, opt):
    """Samples one source clip and returns (times, {target node: [rotation]}, hips translation list or None)."""
    chan_times = sorted({t for ch in list(rot.values()) + list(trans.values()) for t in ch.times})
    if not chan_times:
        return None
    times = chan_times
    pair_map = dict(pairs)  # target node -> source node
    s_hips = src.bones.get("hips")
    d_hips = dst.bones.get("hips")

    # facing correction: a world-space yaw turning the source's forward onto the target's
    ys, yd = face_yaw(src), face_yaw(dst)
    yr = (0.0, 0.0, 0.0, 1.0)
    if ys is not None and yd is not None:
        diff = wrap_angle(yd - ys)
        if abs(diff) > 1e-4:
            yr = qaxis_y(diff)
    yr_inv = qconj(yr)

    scale = opt["scale"]
    if scale is None:
        hs, hd = hips_height(src), hips_height(dst)
        scale = (hd / hs) if hs and hd else 1.0

    s_pos0, s_rot0 = src.rest_world
    d_pos0, d_rot0 = dst.rest_world
    out_rot = {dn: [] for dn in pair_map}
    deltas = []
    for t in times:
        pose = {}
        for n, ch in rot.items():
            pose.setdefault(n, {})["r"] = ch.sample(t, True)
        for n, ch in trans.items():
            pose.setdefault(n, {})["t"] = ch.sample(t, False)
        s_pos, s_rot = src.world_pos_rot(pose)
        # target globals, parents first: mapped bones take the source's world delta, the rest follow their parent
        g = {}
        for i in dst.order:
            if i in pair_map:
                sn = pair_map[i]
                delta = qmul(s_rot[sn], qconj(s_rot0[sn]))
                delta = qmul(qmul(yr, delta), yr_inv)
                g[i] = qnorm(qmul(delta, d_rot0[i]))
            else:
                par = dst.parent[i]
                g[i] = qnorm(qmul(g[par], dst.rest_r[i])) if par >= 0 else d_rot0[i]
        for dn in pair_map:
            par = dst.parent[dn]
            local = qmul(qconj(g[par]), g[dn]) if par >= 0 else g[dn]
            if out_rot[dn] and qdot(out_rot[dn][-1], local) < 0:
                local = tuple(-c for c in local)
            out_rot[dn].append(qnorm(local))
        if d_hips is not None and s_hips is not None and not opt["no_translation"]:
            deltas.append(vscale(qrot(yr, vsub(s_pos[s_hips], s_pos0[s_hips])), scale))
    out_trans = None
    if deltas:
        if opt["in_place_motion"] and len(deltas) > 1:
            # remove the steady horizontal travel: the straight line from the first key's position to the last
            t0, t1 = times[0], times[-1]
            fixed = []
            for t, d in zip(times, deltas):
                f = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
                x = deltas[0][0] + (deltas[-1][0] - deltas[0][0]) * f
                z = deltas[0][2] + (deltas[-1][2] - deltas[0][2]) * f
                fixed.append((d[0] - x, d[1], d[2] - z))
            deltas = fixed
        out_trans = []
        for delta in deltas:
            world = vadd(d_pos0[d_hips], delta)
            par = dst.parent[d_hips]
            if par >= 0:
                # the parent chain is not animated: back to the hips' parent space
                local = qrot(qconj(d_rot0[par]), vsub(world, d_pos0[par]))
                local = vscale(local, 1.0 / (dst.world_scale(par) or 1.0))
            else:
                local = world
            out_trans.append(local)
    return times, out_rot, (out_trans if out_trans else None)


# ---- writing ----------------------------------------------------------------------------------------------

def pad4(b, fill=b"\0"):
    return b + fill * (-len(b) % 4)


class Writer:
    def __init__(self, gltf):
        self.j = gltf.json
        buf = gltf.buffers[0] if gltf.buffers else b""
        if len(gltf.buffers) > 1:
            # merge: every bufferView is repointed into one buffer
            merged = b""
            offsets = []
            for b in gltf.buffers:
                offsets.append(len(merged))
                merged += pad4(b)
            for bv in self.j.get("bufferViews", []):
                bv["byteOffset"] = bv.get("byteOffset", 0) + offsets[bv.get("buffer", 0)]
                bv["buffer"] = 0
            buf = merged
            self.j["buffers"] = [{"byteLength": len(buf)}]
        self.bin = bytearray(buf)
        self.j.setdefault("buffers", [{"byteLength": 0}])
        self.j["buffers"][0].pop("uri", None)
        self.j.setdefault("bufferViews", [])
        self.j.setdefault("accessors", [])
        self.j.setdefault("animations", [])

    def add_accessor(self, data, comp_type, type_name, minmax=False):
        while len(self.bin) % 4:
            self.bin.append(0)
        offset = len(self.bin)
        n = TYPE_COUNT[type_name]
        flat = [c for e in data for c in (e if isinstance(e, tuple) else (e,))]
        self.bin += struct.pack("<" + "f" * len(flat), *flat)
        self.j["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(flat) * 4})
        acc = {"bufferView": len(self.j["bufferViews"]) - 1, "componentType": comp_type, "count": len(data), "type": type_name}
        if minmax:
            cols = list(zip(*[(e if isinstance(e, tuple) else (e,)) for e in data]))
            acc["min"] = [min(c) for c in cols]
            acc["max"] = [max(c) for c in cols]
        self.j["accessors"].append(acc)
        return len(self.j["accessors"]) - 1

    def add_animation(self, name, times, rotations, translations):
        samplers, channels = [], []
        t_acc = self.add_accessor(times, 5126, "SCALAR", minmax=True)
        for node in sorted(rotations):
            o = self.add_accessor(rotations[node], 5126, "VEC4")
            samplers.append({"input": t_acc, "interpolation": "LINEAR", "output": o})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": "rotation"}})
        for node, vals in sorted(translations.items()):
            o = self.add_accessor(vals, 5126, "VEC3")
            samplers.append({"input": t_acc, "interpolation": "LINEAR", "output": o})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": "translation"}})
        self.j["animations"].append({"name": name, "samplers": samplers, "channels": channels})

    def to_glb(self):
        self.j["buffers"][0]["byteLength"] = len(self.bin)
        js = pad4(json.dumps(self.j, separators=(",", ":"), ensure_ascii=False).encode("utf-8"), b" ")
        bn = pad4(bytes(self.bin))
        total = 12 + 8 + len(js) + (8 + len(bn) if bn else 0)
        out = struct.pack("<III", GLB_MAGIC, 2, total) + struct.pack("<II", len(js), CHUNK_JSON) + js
        if bn:
            out += struct.pack("<II", len(bn), CHUNK_BIN) + bn
        return out


# ---- driver -----------------------------------------------------------------------------------------------

class Tool:
    def __init__(self):
        self.warnings = []

    def warn(self, msg):
        if msg in self.warnings:
            return
        self.warnings.append(msg)
        print(f"mce retarget: warning: {msg}", file=sys.stderr)


def describe_unmatched(sk):
    mapped = set(sk.bones.values())
    return [sk.name[i] for i in sorted(sk.joints or range(sk.count)) if i not in mapped]


def require_humanoid(sk, role, tool):
    need = ("hips", "thigh_l", "thigh_r", "upper_arm_l", "upper_arm_r")
    missing = [k for k in need if k not in sk.bones]
    if "hips" in missing or len(sk.bones) < 8:
        names = ", ".join(sk.name[i] for i in sorted(sk.joints or range(sk.count))[:24])
        why = ""
        if not sk.joints:
            why = (" It has no skin: a rig animated through plain nodes (like Kenney's Blocky Characters) has no skeleton "
                   "to recognise, so pair its nodes yourself and add --map-only.")
        raise RetargetError(f"the {role} ({sk.gltf.name}) does not look like a humanoid: found {len(sk.bones)} known bones"
                            f"{', no hips' if 'hips' in missing else ''}.{why} Its nodes: {names}. "
                            "Name the pairs in a --map file ({\"SourceBone\": \"TargetBone\"}).")
    if missing:
        tool.warn(f"the {role} has no {', '.join(missing)}; those limbs are not retargeted")


def retarget(sources, target_path, out_path, opt):
    tool = Tool()
    target = Gltf(target_path)
    dst = Skeleton(target)
    match_humanoid(dst)
    if not opt["map_only"]:
        require_humanoid(dst, "target", tool)
    if opt["map_only"]:
        dst.bones = {}  # no humanoid roles: no facing correction, no hips scaling
    explicit = {}
    if opt.get("map"):
        with open(opt["map"], "r", encoding="utf-8-sig") as f:
            explicit = json.load(f)
        if not isinstance(explicit, dict) or not all(isinstance(v, str) for v in explicit.values()):
            raise RetargetError("--map must be an object of source bone name -> target bone name")
    if os.path.dirname(os.path.abspath(out_path)) != os.path.dirname(os.path.abspath(target_path)) and any(
            im.get("uri") and not im["uri"].startswith("data:") for im in target.json.get("images", [])):
        tool.warn("the character's textures are linked by relative path (" +
                  ", ".join(sorted({im["uri"] for im in target.json["images"] if im.get("uri")})[:3]) +
                  "), but the output is in another folder: write it beside the character, or copy the textures along")
    writer = Writer(target)
    if opt["drop_existing"]:
        writer.j["animations"] = []
    existing = {a.get("name") for a in writer.j["animations"]}
    prefix = opt["prefix"]
    wanted = set(opt["clips"]) if opt["clips"] else None
    seen_wanted = set()
    added = []
    for sp in sources:
        gltf = Gltf(sp)
        src = Skeleton(gltf)
        match_humanoid(src)
        if opt["map_only"]:
            src.bones = {}
        if not opt["map_only"] and not explicit:
            require_humanoid(src, "source", tool)
        pairs = build_pairs(src, dst, explicit, auto=not opt["map_only"])
        if len(pairs) < 4:
            raise RetargetError(f"only {len(pairs)} bones of {gltf.name} match the target: nothing to retarget")
        mapped_src = {s for _, s in pairs}
        for ai, anim in enumerate(gltf.json.get("animations", [])):
            cname = anim.get("name") or f"{os.path.splitext(gltf.name)[0]}_{ai}"
            if wanted is not None:
                if cname not in wanted:
                    continue
                seen_wanted.add(cname)
            rot, trans, ignored = read_clip(gltf, anim, tool.warn)
            if ignored:
                tool.warn(f"clip '{cname}': {ignored} scale / morph-weight channel(s) are not carried")
            hips_chain = set()
            h = src.bones.get("hips")
            while h is not None and h >= 0:
                hips_chain.add(h)
                h = src.parent[h]
            orphans = sorted({src.name[n] for n in list(rot) + list(trans)
                              if n not in mapped_src and n not in hips_chain and (not src.joints or n in src.joints)})
            if orphans:
                tool.warn(f"clip '{cname}': animated source bones with no counterpart on the target: "
                          + ", ".join(orphans[:12]) + (" ..." if len(orphans) > 12 else ""))
            res = retarget_clip(src, dst, pairs, rot, trans, opt)
            if res is None:
                tool.warn(f"clip '{cname}' of {gltf.name} has no keys; skipped")
                continue
            times, out_rot, out_trans = res
            final = prefix + cname
            if final in existing:
                if opt["replace"]:
                    writer.j["animations"] = [a for a in writer.j["animations"] if a.get("name") != final]
                else:
                    tool.warn(f"clip '{final}' already exists in the output; skipped (--replace overwrites, --prefix renames)")
                    continue
            hips = dst.bones.get("hips")
            tr = {hips: out_trans} if out_trans is not None and hips is not None else {}
            writer.add_animation(final, times, out_rot, tr)
            existing.add(final)
            added.append(final)
    if wanted is not None and wanted - seen_wanted:
        raise RetargetError("no clip named " + ", ".join(sorted(wanted - seen_wanted)) + " in the sources")
    if not added:
        raise RetargetError("no clip was retargeted")
    unmatched_dst = describe_unmatched(dst)
    if unmatched_dst:
        tool.warn(f"{len(unmatched_dst)} target bone(s) have no humanoid role and stay at their rest pose: "
                  + ", ".join(unmatched_dst[:12]) + (" ..." if len(unmatched_dst) > 12 else ""))
    if opt["strict"] and tool.warnings:
        raise RetargetError(f"--strict: {len(tool.warnings)} warning(s); nothing written")
    data = writer.to_glb()
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(data)
    return added


def write_sidecar(target_path, out_path):
    """A new output next to a project's assets gets the target's importer settings and a fresh GUID."""
    side = out_path + ".mceasset"
    if os.path.exists(side):
        return
    settings = {}
    t_side = target_path + ".mceasset"
    if os.path.isfile(t_side):
        try:
            with open(t_side, "r", encoding="utf-8-sig") as f:
                settings = json.load(f).get("settings", {})
        except (OSError, ValueError):
            pass
    guid = str(uuid.uuid5(uuid.NAMESPACE_URL, "mce-retarget/" + os.path.basename(out_path)))
    with open(side, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"guid": guid, "importer": "gltf", "importer_version": 1, "settings": settings}, f, indent=2)
        f.write("\n")
    print(f"wrote {side}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mce retarget", description="retarget humanoid animation clips onto a character's skeleton")
    ap.add_argument("sources", nargs="+", help="glTF files with the clips (an animation library, a Mixamo export, ...)")
    ap.add_argument("--onto", required=True, help="the character .glb that receives the clips")
    ap.add_argument("--out", help="output .glb (default: <character>_retargeted.glb beside it)")
    ap.add_argument("--inplace", action="store_true", help="overwrite the character file")
    ap.add_argument("--clips", help="only these clip names, comma separated")
    ap.add_argument("--prefix", default="", help="prepended to every clip name")
    ap.add_argument("--map", help="JSON file {\"SourceBone\": \"TargetBone\"} for pairs the names do not find")
    ap.add_argument("--map-only", action="store_true",
                    help="pair only what --map names (no humanoid name matching): for rigs animated through plain nodes")
    ap.add_argument("--replace", action="store_true", help="a clip with the name of one already in the character replaces it")
    ap.add_argument("--drop-existing", action="store_true", help="remove the character's own clips")
    ap.add_argument("--in-place-motion", action="store_true", help="drop the hips' horizontal travel (the game moves the character)")
    ap.add_argument("--no-translation", action="store_true", help="rotations only: the hips stay at their rest position")
    ap.add_argument("--scale", default="auto", help="hips translation scale: auto (hip-height ratio) or a number")
    ap.add_argument("--strict", action="store_true", help="any warning is a failure and nothing is written")
    args = ap.parse_args(argv)
    if args.inplace and args.out:
        ap.error("--inplace and --out are exclusive")
    try:
        scale = None if args.scale == "auto" else float(args.scale)
    except ValueError:
        ap.error("--scale must be 'auto' or a number")
    if args.map_only and not args.map:
        ap.error("--map-only needs --map")
    out = args.onto if args.inplace else (args.out or os.path.splitext(args.onto)[0] + "_retargeted.glb")
    if not out.lower().endswith(".glb"):
        ap.error("the output must be a .glb")
    opt = {"clips": [c for c in (args.clips or "").split(",") if c], "prefix": args.prefix, "map": args.map,
           "replace": args.replace, "map_only": args.map_only, "drop_existing": args.drop_existing, "in_place_motion": args.in_place_motion,
           "no_translation": args.no_translation, "scale": scale, "strict": args.strict}
    try:
        added = retarget(args.sources, args.onto, out, opt)
    except RetargetError as e:
        print(f"mce retarget: {e}", file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, struct.error) as e:
        print(f"mce retarget: {type(e).__name__}: {e}", file=sys.stderr)
        return 1
    if not args.inplace:
        write_sidecar(args.onto, out)
    print(f"wrote {out} ({len(added)} clips: {', '.join(added[:8])}{' ...' if len(added) > 8 else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
