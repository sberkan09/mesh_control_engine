#!/usr/bin/env python3
"""mce import spine / dragonbones: 2D skeleton exports into a .mcskel2d (M-IMPORT, M-ANIM2D; SBA-146).

    mce import spine       <export.json>  [--out f.mcskel2d] [--name n] [--ppm 100] [--atlas f.atlas]
                           [--texture f.png] [--strict]
    mce import dragonbones <skeleton.json> [--out f.mcskel2d] [--name n] [--ppm 100] [--atlas f_tex.json]
                           [--texture f.png] [--armature name] [--strict]

spine:        the open JSON export of Spine 3.8 or 4.x (not the .skel binary). Only the JSON is read; no
              Spine runtime code is used or shipped.
dragonbones:  the DragonBones 5.x JSON skeleton (_ske.json). The binary .dbbin is not read.

Both produce the same file: bones, slots (draw order, color), region and mesh attachments (weighted meshes
included), bone rotate / translate / scale keys, slot color and attachment keys, mesh deform keys, events,
and IK constraints with keyed mix. Units: pixels become metres (--ppm pixels per metre, default 100), Y
points up, rotation is degrees counter-clockwise (DragonBones' Y-down, clockwise data is flipped).

--atlas (Spine: the libGDX .atlas text; DragonBones: the texture atlas _tex.json) gives each attachment its
rect in --texture (the atlas page PNG). The PNG gets a texture sidecar when it has none. Without an atlas
every attachment uses the whole texture; without a texture the quads are solid.

Whatever the engine cannot hold is dropped with a warning on stderr that names it (path, transform and
physics constraints, shear, other skins, draw-order keys, blend modes, two-color slots, ...) and the exit
code stays 0; --strict makes any warning a failure and nothing is written. Re-importing over a file keeps
its GUID, and the output is a pure function of the inputs.
"""

import argparse
import json
import math
import os
import sys
import uuid

MAX_BONES = 64
MAX_SLOTS = 256
MAX_IK = 16
MAX_NAME = 31
MAX_EVENT_NAME = 23
MAX_SLOT_ATTACHMENTS = 64
MAX_MESH_VERTICES = 1024
MAX_MESH_TRIANGLES = 2048
MAX_ANIMATIONS = 256
FLIP_STEP = 1e-9


class Importer:
    def __init__(self, tool):
        self.tool = tool
        self.warnings = []

    def warn(self, msg):
        if msg in self.warnings:
            return
        self.warnings.append(msg)
        print(f"mce import {self.tool}: warning: {msg}", file=sys.stderr)


def read_json(path):
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8-sig"))


def existing_guid(path):
    try:
        g = read_json(path).get("guid")
        return g if isinstance(g, str) and g else None
    except (OSError, ValueError, AttributeError):
        return None


def num(x, nd=6):
    v = round(float(x), nd)
    if v == 0:
        return 0
    return int(v) if v == int(v) else v


def wrap180(d):
    return d - 360.0 * math.floor((d + 180.0) / 360.0)


class NameSet:
    """Engine names are 1..31 characters (events 23) and unique per kind."""

    def __init__(self, imp, what, limit=MAX_NAME):
        self.imp, self.what, self.limit, self.taken = imp, what, limit, set()

    def make(self, raw):
        raw = "unnamed" if raw in (None, "") else str(raw)
        base = raw[:self.limit]
        if base != raw:
            self.imp.warn(f"{self.what} name '{raw}' is longer than {self.limit} characters; cut to '{base}'")
        out, n = base, 2
        while out in self.taken:
            suffix = f"_{n}"
            out = base[:self.limit - len(suffix)] + suffix
            n += 1
        if out != base:
            self.imp.warn(f"two {self.what}s are named '{base}'; the second is '{out}'")
        self.taken.add(out)
        return out


# --- atlases -----------------------------------------------------------------------------------------------

class Region:
    """A packed image: x, y, w, h on the page (top-left origin, pixels), the original size ow x oh and the
    offset ox, oy of the packed part inside it (from the original's bottom-left)."""

    def __init__(self, page_w, page_h, x, y, w, h, ow=None, oh=None, ox=0.0, oy=0.0, rotated=False):
        self.pw, self.ph, self.x, self.y, self.w, self.h = page_w, page_h, x, y, w, h
        self.ow = ow if ow else w
        self.oh = oh if oh else h
        self.ox, self.oy, self.rotated = ox, oy, rotated

    def rect(self):
        return [num(self.x / self.pw, 6), num(self.y / self.ph, 6),
                num((self.x + self.w) / self.pw, 6), num((self.y + self.h) / self.ph, 6)]


def parse_libgdx_atlas(path, imp):
    """name -> Region of the first page of a libGDX/Spine .atlas (old and 4.x property spellings)."""
    with open(path, "rb") as f:
        lines = f.read().decode("utf-8-sig").splitlines()
    pages, regions = [], {}
    page = cur = None
    expect_page = True
    for raw in lines:
        line = raw.strip()
        if not line:
            expect_page, cur = True, None
            continue
        if expect_page:
            page = {"file": line, "w": 0, "h": 0}
            pages.append(page)
            expect_page, cur = False, None
            continue
        if ":" not in line:
            cur = {"name": line, "page": len(pages) - 1, "x": 0, "y": 0, "w": 0, "h": 0, "rotate": False}
            regions.setdefault(line, cur)
            continue
        key, val = line.split(":", 1)
        key = key.strip()
        v = [s.strip() for s in val.split(",")]
        try:
            if cur is None:
                if key == "size":
                    page["w"], page["h"] = float(v[0]), float(v[1])
            elif key == "xy":
                cur["x"], cur["y"] = float(v[0]), float(v[1])
            elif key == "size":
                cur["w"], cur["h"] = float(v[0]), float(v[1])
            elif key == "bounds":
                cur["x"], cur["y"], cur["w"], cur["h"] = (float(s) for s in v[:4])
            elif key == "orig":
                cur["ow"], cur["oh"] = float(v[0]), float(v[1])
            elif key == "offset":
                cur["ox"], cur["oy"] = float(v[0]), float(v[1])
            elif key == "offsets":
                cur["ox"], cur["oy"], cur["ow"], cur["oh"] = (float(s) for s in v[:4])
            elif key == "rotate":
                cur["rotate"] = v[0].lower() not in ("false", "0")
        except (ValueError, IndexError):
            raise ValueError(f"{path}: cannot read '{line}'") from None
    if not pages:
        raise ValueError(f"{path}: no atlas page found")
    if len(pages) > 1:
        imp.warn(f"the atlas has {len(pages)} pages; only the first ('{pages[0]['file']}') is used, regions on "
                 "the others are drawn from the whole texture")
    out = {}
    for name, r in regions.items():
        if r["page"] != 0:
            continue
        p = pages[0]
        if p["w"] < 1 or p["h"] < 1:
            raise ValueError(f"{path}: the page has no size")
        if r["w"] < 1 or r["h"] < 1:
            raise ValueError(f"{path}: region '{name}' has no size")
        out[name] = Region(p["w"], p["h"], r["x"], r["y"], r["w"], r["h"], r.get("ow"), r.get("oh"),
                           r.get("ox", 0.0), r.get("oy", 0.0), r["rotate"])
    return out


def parse_db_atlas(path, imp):
    """name -> Region of a DragonBones texture atlas JSON (SubTexture list)."""
    doc = read_json(path)
    pw, ph = float(doc.get("width", 0)), float(doc.get("height", 0))
    if pw < 1 or ph < 1:
        raise ValueError(f"{path}: the atlas needs width and height")
    out = {}
    for s in doc.get("SubTexture") or []:
        w, h = float(s["width"]), float(s["height"])
        fw, fh = float(s.get("frameWidth", w)), float(s.get("frameHeight", h))
        fx, fy = float(s.get("frameX", 0)), float(s.get("frameY", 0))
        # frameX / frameY are the (negative) top-left of the frame relative to the packed image: the packed
        # part sits at (-fx, -fy) from the frame's top-left; Region offsets count from the bottom-left.
        out.setdefault(s["name"], Region(pw, ph, float(s["x"]), float(s["y"]), w, h, fw, fh, -fx, fh + fy - h,
                                         bool(s.get("rotated", False))))
    return out


# --- curves, keys --------------------------------------------------------------------------------------------

def clean_bezier(c, imp, where):
    cx1, cy1, cx2, cy2 = c
    if not (-1e-4 <= cx1 <= 1 + 1e-4 and -1e-4 <= cx2 <= 1 + 1e-4):
        imp.warn(f"{where}: a bezier control point lies outside the key interval in time; clamped to 0..1")
    cx1, cx2 = min(max(cx1, 0.0), 1.0), min(max(cx2, 0.0), 1.0)
    return [num(cx1, 5), num(cy1, 5), num(cx2, 5), num(cy2, 5)]


def spine_curve(key, t1, t2, chans, imp, where):
    """Spine key curve -> engine curve (None: linear). 3.8: curve, c2, c3, c4 already normalized. 4.x: a list
    of four numbers per channel in absolute time and value, normalized here by the key interval."""
    c = key.get("curve")
    if c is None:
        return None
    if c == "stepped":
        return "stepped"
    if isinstance(c, (int, float)):
        return clean_bezier([float(c), float(key.get("c2", 0)), float(key.get("c3", 1)), float(key.get("c4", 1))],
                            imp, where)
    if isinstance(c, list):
        dt = t2 - t1
        cands = []
        for i, (v1, v2) in enumerate(chans):
            seg = c[4 * i:4 * i + 4]
            if len(seg) < 4 or dt <= 0 or abs(v2 - v1) < 1e-9:
                continue
            dv = v2 - v1
            cands.append([(seg[0] - t1) / dt, (seg[1] - v1) / dv, (seg[2] - t1) / dt, (seg[3] - v1) / dv])
        if not cands:
            return None
        if any(max(abs(a - b) for a, b in zip(cands[0], o)) > 1e-3 for o in cands[1:]):
            imp.warn(f"{where}: the channels of a key have different curves; the first one is used for all")
        return clean_bezier(cands[0], imp, where)
    imp.warn(f"{where}: unknown curve {c!r}; linear")
    return None


def increasing(pts, imp, where):
    """Drop points whose time does not increase (the cook needs strictly increasing key times)."""
    out = []
    for p in pts:
        if p[0] < 0:
            imp.warn(f"{where}: a key at negative time {p[0]} is dropped")
            continue
        if out and not p[0] > out[-1][0]:
            imp.warn(f"{where}: a key at t={p[0]} does not come after the one before; dropped")
            continue
        out.append(p)
    return out


def build_keys(pts, curvefn, fmt):
    """pts: [(t, [channel values], source key)]; fmt(t, vals) -> the engine key."""
    out = []
    for i, (t, vals, src) in enumerate(pts):
        d = fmt(t, vals)
        if i + 1 < len(pts):
            cv = curvefn(src, t, pts[i + 1][0], list(zip(vals, pts[i + 1][1])))
            if cv is not None:
                d["curve"] = cv
        out.append(d)
    return out


def unwrap(vals):
    """Make a rotation sequence continuous the short way round; returns (values, changed)."""
    out = [vals[0]]
    for i in range(1, len(vals)):
        out.append(out[-1] + wrap180(vals[i] - vals[i - 1]))
    return out, any(abs(a - b) > 1e-9 for a, b in zip(out, vals))


# --- shared builders -------------------------------------------------------------------------------------------

def build_quad(name, tex, reg, width, height, cx, cy, rot, sx, sy, ppm, imp, what):
    """A quad attachment. width x height: the nominal image size; (cx, cy): the centre in the bone's space (pixels,
    Y up); rot: degrees counter-clockwise; sx, sy: scale. A trimmed atlas region sits inside its original."""
    if sx < 0 or sy < 0:
        imp.warn(f"{what}: a negative scale (a mirrored image) is not supported; its size is used as positive")
    if reg is not None and reg.rotated:
        imp.warn(f"{what}: the atlas region is stored rotated, which is not supported; repack with rotation off")
    q = {"name": name}
    shx = shy = 0.0
    if reg is not None:
        fx = (width / reg.ow) if width else 1.0
        fy = (height / reg.oh) if height else 1.0
        size = (reg.w * fx * abs(sx), reg.h * fy * abs(sy))
        shx = (reg.ox + reg.w / 2.0 - reg.ow / 2.0) * fx * sx
        shy = (reg.oy + reg.h / 2.0 - reg.oh / 2.0) * fy * sy
    else:
        size = (width * abs(sx), height * abs(sy))
    if tex:
        q["texture"] = tex
        if reg is not None:
            q["rect"] = reg.rect()
    q["size"] = [num(size[0] / ppm), num(size[1] / ppm)]
    r = math.radians(rot)
    ox = cx + shx * math.cos(r) - shy * math.sin(r)
    oy = cy + shx * math.sin(r) + shy * math.cos(r)
    if abs(ox) > FLIP_STEP or abs(oy) > FLIP_STEP:
        q["offset"] = [num(ox / ppm), num(oy / ppm)]
    if abs(rot) > FLIP_STEP:
        q["rotation"] = num(rot)
    return q


def uv_in(reg, u, v):
    if reg is None:
        return [num(u, 6), num(v, 6)]
    return [num((reg.x + u * reg.w) / reg.pw, 6), num((reg.y + v * reg.h) / reg.ph, 6)]


class Att:
    """What the animations need to know about an emitted attachment."""

    def __init__(self, name, mesh=False, nverts=0, infl=None):
        self.name, self.mesh, self.nverts, self.infl = name, mesh, nverts, infl  # infl: per vertex, all weights


def finish_mesh(name, tex, uvs, verts, tris, imp, what):
    """verts: per vertex a list of (bone name, x, y, weight) already in metres, or one (None, x, y, 1) for a
    vertex in the slot bone's space. Returns the attachment dict, or None when the engine cannot hold it."""
    n = len(verts)
    if n < 3 or n > MAX_MESH_VERTICES:
        imp.warn(f"{what}: a mesh of {n} vertices is not supported (3 to {MAX_MESH_VERTICES}); the attachment is dropped")
        return None
    if len(tris) < 3 or len(tris) % 3 or len(tris) // 3 > MAX_MESH_TRIANGLES or max(tris) >= n or min(tris) < 0:
        imp.warn(f"{what}: the mesh's triangles are not usable (1 to {MAX_MESH_TRIANGLES}, in range); the attachment is dropped")
        return None
    out_v = []
    for uv, infl in zip(uvs, verts):
        v = {"uv": uv}
        if infl[0][0] is None:
            v["x"], v["y"] = num(infl[0][1]), num(infl[0][2])
        else:
            if len(infl) > 4:
                imp.warn(f"{what}: a vertex follows {len(infl)} bones; the 4 heaviest are kept")
                infl = sorted(infl, key=lambda w: -w[3])[:4]
            infl = [w for w in infl if w[3] > 0] or infl[:1]
            if sum(w[3] for w in infl) <= 0:
                infl = [(infl[0][0], infl[0][1], infl[0][2], 1.0)]
            v["weights"] = [{"bone": b, "x": num(x), "y": num(y), "weight": num(w, 6)} for b, x, y, w in infl]
        out_v.append(v)
    m = {"name": name, "type": "mesh"}
    if tex:
        m["texture"] = tex
    m["vertices"] = out_v
    m["triangles"] = [int(i) for i in tris]
    return m


def deform_offsets(att, flat, start, ppm, flip_y):
    """Per-vertex [dx, dy, ...] in metres from a deform key's values. A weighted vertex has one delta per
    bone it follows (in that bone's space); the engine takes one per vertex, so the weighted average."""
    out = []
    i = 0
    for vi in range(att.nverts):
        if att.infl is None:
            n, ws = 1, [1.0]
        else:
            ws = att.infl[vi]
            n = len(ws)
        sx = sy = 0.0
        tot = sum(ws) or 1.0
        for j in range(n):
            k = i - start
            dx = flat[k] if 0 <= k < len(flat) else 0.0
            dy = flat[k + 1] if 0 <= k + 1 < len(flat) else 0.0
            i += 2
            sx += dx * ws[j] / tot
            sy += dy * ws[j] / tot
        out += [num(sx / ppm), num((-sy if flip_y else sy) / ppm)]
    return out


def valid_ik(imp, name, chain, target, parents):
    """Check a chain against the cook's rules; returns True when it can be written."""
    for a, b in zip(chain, chain[1:]):
        if parents.get(b) != a:
            imp.warn(f"IK constraint '{name}': '{b}' is not a child of '{a}'; dropped")
            return False
    if target is not None:
        x = target
        while x is not None:
            if x == chain[0]:
                imp.warn(f"IK constraint '{name}': the target bone '{target}' is under the chain's first bone; dropped")
                return False
            x = parents.get(x)
    return True


def finish_bones(imp, bones):
    if not bones:
        raise ValueError("the skeleton has no bones")
    if len(bones) > MAX_BONES:
        raise ValueError(f"the skeleton has {len(bones)} bones; the engine allows {MAX_BONES}")


# --- Spine ---------------------------------------------------------------------------------------------------------

def spine_hex(s):
    s = str(s).lstrip("#")
    if len(s) == 6:
        s += "ff"
    if len(s) != 8:
        raise ValueError(f"bad color '{s}'")
    return [num(int(s[i:i + 2], 16) / 255.0, 5) for i in (0, 2, 4, 6)]


def convert_spine(doc, o, imp):
    ppm = o["ppm"]
    sk = doc.get("skeleton") or {}
    ver = str(sk.get("spine", ""))
    ver3 = ver.startswith("3.")
    if ver and not (ver.startswith("3.8") or ver.startswith("4.")):
        imp.warn(f"Spine {ver} is not 3.8 or 4.x; the JSON is read as 3.8/4.x and may be wrong")
    atlas, tex = o["atlas"], o["tex"]

    # bones
    bones_in = doc.get("bones") or []
    finish_bones(imp, bones_in)
    bnames = NameSet(imp, "bone")
    bmap = {}
    bones = []
    parents = {}
    for i, b in enumerate(bones_in):
        raw = b.get("name")
        parent = b.get("parent")
        if i == 0 and parent:
            raise ValueError("the first bone must be the root")
        if i > 0 and parent not in bmap:
            raise ValueError(f"bone '{raw}' names parent '{parent}', which is not listed before it")
        nm = bnames.make(raw)
        bmap[raw] = nm
        parents[nm] = bmap[parent] if parent else None
        e = {"name": nm}
        if parent:
            e["parent"] = bmap[parent]
        x, y = float(b.get("x", 0)) / ppm, float(b.get("y", 0)) / ppm
        if abs(x) > FLIP_STEP:
            e["x"] = num(x)
        if abs(y) > FLIP_STEP:
            e["y"] = num(y)
        if abs(float(b.get("rotation", 0))) > FLIP_STEP:
            e["rotation"] = num(b["rotation"])
        sx, sy = float(b.get("scaleX", 1)), float(b.get("scaleY", 1))
        if abs(sx - 1) > FLIP_STEP or abs(sy - 1) > FLIP_STEP:
            e["scale"] = [num(sx), num(sy)]
        if float(b.get("length", 0)) > 0:
            e["length"] = num(float(b["length"]) / ppm)
        if float(b.get("shearX", 0)) or float(b.get("shearY", 0)):
            imp.warn(f"bone '{raw}' has shear, which is not supported; dropped")
        mode = b.get("inherit", b.get("transform", "normal"))
        if mode != "normal":
            imp.warn(f"bone '{raw}' inherits with '{mode}', which is not supported; treated as normal")
        if b.get("skin"):
            imp.warn(f"bone '{raw}' is only active with certain skins; it is always active here")
        bones.append(e)

    # skins
    skins = doc.get("skins") or []
    if isinstance(skins, dict):  # Spine before 3.6: {name: attachments}
        skins = [{"name": k, "attachments": v} for k, v in skins.items()]
    default = {}
    for s in skins:
        if s.get("name", "default") == "default":
            default = s.get("attachments") or {}
        else:
            imp.warn(f"skin '{s.get('name')}' is dropped; only the default skin is imported")

    # slots
    slots_in = doc.get("slots") or []
    if len(slots_in) > MAX_SLOTS:
        raise ValueError(f"the skeleton has {len(slots_in)} slots; the engine allows {MAX_SLOTS}")
    snames = NameSet(imp, "slot")
    smap, slots = {}, []
    atts = {}  # engine slot name -> {source attachment key: Att}
    att_names = {}  # engine slot name -> {source key: engine name}
    for s in slots_in:
        raw = s.get("name")
        nm = snames.make(raw)
        smap[raw] = nm
        if s.get("bone") not in bmap:
            raise ValueError(f"slot '{raw}' names bone '{s.get('bone')}', which does not exist")
        e = {"name": nm, "bone": bmap[s["bone"]]}
        color = spine_hex(s["color"]) if s.get("color") else [1, 1, 1, 1]
        if color != [1, 1, 1, 1]:
            e["tint"] = color
        if s.get("dark"):
            imp.warn(f"slot '{raw}' has a dark (two-color tint) color, which is not supported; dropped")
        if s.get("blend", "normal") != "normal":
            imp.warn(f"slot '{raw}' blends '{s['blend']}', which is not supported; drawn normally")
        # attachments
        a_in = default.get(raw) or {}
        names = NameSet(imp, "attachment")
        lst, ainfo, amap = [], {}, {}
        for key, a in a_in.items():
            typ = a.get("type", "region")
            what = f"slot '{raw}', attachment '{key}'"
            if len(lst) >= MAX_SLOT_ATTACHMENTS:
                imp.warn(f"slot '{raw}' has more than {MAX_SLOT_ATTACHMENTS} attachments; '{key}' is dropped")
                continue
            path = a.get("path", a.get("name", key))
            reg = atlas.get(path) if atlas is not None else None
            if atlas is not None and reg is None and typ in ("region", "mesh"):
                imp.warn(f"{what}: region '{path}' is not in the atlas; the whole texture is used")
            if a.get("color") not in (None, "ffffffff"):
                imp.warn(f"{what}: the attachment color is not supported; dropped")
            if typ == "region":
                q = build_quad(names.make(key), tex, reg, float(a.get("width", 0)) or (reg.ow if reg else 0),
                               float(a.get("height", 0)) or (reg.oh if reg else 0),
                               float(a.get("x", 0)), float(a.get("y", 0)), float(a.get("rotation", 0)),
                               float(a.get("scaleX", 1)), float(a.get("scaleY", 1)), ppm, imp, what)
                if q["size"] == [0, 0]:
                    imp.warn(f"{what}: the region has no size; dropped")
                    continue
                lst.append(q)
                amap[key] = q["name"]
                ainfo[key] = Att(q["name"])
            elif typ == "mesh":
                uvs_in = a.get("uvs") or []
                vin = a.get("vertices") or []
                n = len(uvs_in) // 2
                uvs = [uv_in(reg, uvs_in[2 * i], uvs_in[2 * i + 1]) for i in range(n)]
                verts, infl_all = [], None
                if len(vin) == 2 * n:
                    for i in range(n):
                        verts.append([(None, float(vin[2 * i]) / ppm, float(vin[2 * i + 1]) / ppm, 1.0)])
                else:
                    infl_all, i = [], 0
                    for _ in range(n):
                        cnt = int(vin[i])
                        i += 1
                        infl, ws = [], []
                        for _ in range(cnt):
                            bi, bx, by, w = int(vin[i]), float(vin[i + 1]), float(vin[i + 2]), float(vin[i + 3])
                            i += 4
                            infl.append((bmap[bones_in[bi]["name"]], bx / ppm, by / ppm, w))
                            ws.append(w)
                        verts.append(infl)
                        infl_all.append(ws)
                m = finish_mesh(names.make(key), tex, uvs, verts, a.get("triangles") or [], imp, what)
                if m is None:
                    continue
                lst.append(m)
                amap[key] = m["name"]
                ainfo[key] = Att(m["name"], True, n, infl_all)
            else:
                imp.warn(f"{what}: a '{typ}' attachment is not supported; dropped")
        shown = s.get("attachment")
        if lst:
            e["attachments"] = lst
            e["attachment"] = amap.get(shown) if shown else None
            if shown and shown not in amap:
                imp.warn(f"slot '{raw}' starts with attachment '{shown}', which was dropped; the slot starts hidden")
        else:
            e["attachment"] = None
        slots.append(e)
        atts[nm] = ainfo
        att_names[nm] = amap

    # constraints
    for kind in ("transform", "path", "physics"):
        for c in doc.get(kind) or []:
            imp.warn(f"{kind} constraint '{c.get('name', '?')}' is not supported; dropped")
    iks, ik_map = [], {}
    inames = NameSet(imp, "IK constraint")
    for c in doc.get("ik") or []:
        raw = c.get("name")
        chain_raw = c.get("bones") or []
        if len(iks) >= MAX_IK:
            imp.warn(f"IK constraint '{raw}': the engine allows {MAX_IK} constraints; dropped")
            continue
        if len(chain_raw) not in (1, 2) or any(b not in bmap for b in chain_raw) or c.get("target") not in bmap:
            imp.warn(f"IK constraint '{raw}': needs 1 or 2 known bones and a known target; dropped")
            continue
        chain = [bmap[b] for b in chain_raw]
        target = bmap[c["target"]]
        if not valid_ik(imp, raw, chain, target, parents):
            continue
        for k in ("softness", "compress", "stretch", "uniform"):
            if c.get(k):
                imp.warn(f"IK constraint '{raw}': '{k}' is not supported; dropped")
        e = {"name": inames.make(raw), "bones": chain, "target": target}
        if len(chain) == 2:
            e["type"] = "two_bone"
            e["bend"] = "positive" if c.get("bendPositive", True) else "negative"
        else:
            e["type"] = "ccd"
        if float(c.get("mix", 1)) != 1:
            e["mix"] = num(c["mix"], 5)
        if int(c.get("order", 0)):
            e["order"] = int(c["order"])
        ik_map[raw] = e
        iks.append(e)
    sevents = doc.get("events") or {}
    for en, ed in sevents.items():
        if isinstance(ed, dict) and any(ed.get(k) for k in ("int", "float", "string", "audio")):
            imp.warn(f"event '{en}': its payload (int, float, string, audio) is not supported; only the name is kept")

    # animations
    anims = []
    anames = NameSet(imp, "animation")
    for aname, a in (doc.get("animations") or {}).items():
        if len(anims) >= MAX_ANIMATIONS:
            imp.warn(f"more than {MAX_ANIMATIONS} animations; '{aname}' and the rest are dropped")
            break
        out = {"name": anames.make(aname)}
        where0 = f"animation '{aname}'"
        tmax = 0.0

        def times(lst):
            nonlocal tmax
            for k in lst or []:
                tmax = max(tmax, float(k.get("time", 0)))

        def curvefn(where):
            return lambda src, t1, t2, chans: spine_curve(src, t1, t2, chans, imp, where)

        bones_out = {}
        for braw, tl in (a.get("bones") or {}).items():
            where = f"{where0}, bone '{braw}'"
            for kk in tl:
                times(tl[kk])
            if braw not in bmap:
                imp.warn(f"{where}: no such bone; its keys are dropped")
                continue
            ch = {}
            for kk in ("shear", "shearX", "shearY", "translatex", "translatey", "scalex", "scaley", "shearx", "sheary"):
                if tl.get(kk):
                    imp.warn(f"{where}: '{kk}' timelines are not supported; dropped")
            if tl.get("rotate"):
                pts = increasing([(float(k.get("time", 0)), [float(k.get("value", k.get("angle", 0)))], k) for k in tl["rotate"]],
                                 imp, where)
                if ver3 and pts:
                    vals, changed = unwrap([p[1][0] for p in pts])
                    if changed:
                        imp.warn(f"{where}: Spine 3.x turns the short way between rotate keys; the keys are unwound to match")
                    pts = [(p[0], [v], p[2]) for p, v in zip(pts, vals)]
                if pts:
                    ch["rotate"] = build_keys(pts, curvefn(where), lambda t, v: {"t": num(t, 5), "value": num(v[0], 5)})
            if tl.get("translate"):
                pts = increasing([(float(k.get("time", 0)), [float(k.get("x", 0)), float(k.get("y", 0))], k)
                                  for k in tl["translate"]], imp, where)
                if pts:
                    ch["translate"] = build_keys(pts, curvefn(where),
                                                 lambda t, v: {"t": num(t, 5), "x": num(v[0] / ppm), "y": num(v[1] / ppm)})
            if tl.get("scale"):
                pts = increasing([(float(k.get("time", 0)), [float(k.get("x", 1)), float(k.get("y", 1))], k)
                                  for k in tl["scale"]], imp, where)
                if pts:
                    ch["scale"] = build_keys(pts, curvefn(where),
                                             lambda t, v: {"t": num(t, 5), "x": num(v[0], 5), "y": num(v[1], 5)})
            if ch:
                bones_out[bmap[braw]] = ch
        if bones_out:
            out["bones"] = bones_out

        slots_out = {}
        for sraw, tl in (a.get("slots") or {}).items():
            where = f"{where0}, slot '{sraw}'"
            for kk in tl:
                times(tl[kk])
            if sraw not in smap:
                imp.warn(f"{where}: no such slot; its keys are dropped")
                continue
            sn = smap[sraw]
            ch = {}
            for kk in ("twoColor", "rgb2", "rgba2", "rgb", "alpha"):
                if tl.get(kk):
                    imp.warn(f"{where}: '{kk}' timelines are not supported (key 'rgba' or 'color' instead); dropped")
            if tl.get("attachment"):
                if not att_names[sn]:
                    imp.warn(f"{where}: the slot has no attachment here; attachment keys dropped")
                else:
                    pts = increasing([(float(k.get("time", 0)), [], k) for k in tl["attachment"]], imp, where)
                    keys = []
                    for t, _, k in pts:
                        nm = k.get("name")
                        if nm is not None and nm not in att_names[sn]:
                            imp.warn(f"{where}: attachment '{nm}' is not available; the slot is hidden at t={t}")
                            nm = None
                        keys.append({"t": num(t, 5), "name": att_names[sn].get(nm) if nm is not None else None})
                    if keys:
                        ch["attachment"] = keys
            ck = tl.get("rgba") or tl.get("color")
            if ck:
                pts = increasing([(float(k.get("time", 0)), spine_hex(k.get("color", "ffffffff")), k) for k in ck], imp, where)
                if pts:
                    ch["color"] = build_keys(pts, curvefn(where), lambda t, v: {"t": num(t, 5), "color": v})
            if ch:
                slots_out[sn] = ch
        if slots_out:
            out["slots"] = slots_out

        for skin_name, per_slot in (a.get("deform") or {}).items():
            if skin_name != "default":
                imp.warn(f"{where0}: deform keys of skin '{skin_name}' are dropped; only the default skin is imported")
                continue
            deform = {}
            for sraw, per_att in per_slot.items():
                for araw, keys in per_att.items():
                    where = f"{where0}, slot '{sraw}', attachment '{araw}'"
                    times(keys)
                    info = atts.get(smap.get(sraw), {}).get(araw)
                    if info is None or not info.mesh:
                        imp.warn(f"{where}: no mesh to deform here; its keys are dropped")
                        continue
                    pts = increasing([(float(k.get("time", 0)), [0.0], k) for k in keys], imp, where)
                    pts = [(t, v, k) for t, v, k in pts]
                    out_keys = []
                    for i, (t, _, k) in enumerate(pts):
                        d = {"t": num(t, 5)}
                        offs = deform_offsets(info, [float(x) for x in k.get("vertices") or []], int(k.get("offset", 0)), ppm, False)
                        if any(offs):
                            d["offsets"] = offs
                        if i + 1 < len(pts):
                            cv = spine_curve(k, t, pts[i + 1][0], [(0.0, 1.0)], imp, where)
                            if cv is not None:
                                d["curve"] = cv
                        out_keys.append(d)
                    if out_keys:
                        if info.infl is not None:
                            imp.warn(f"{where}: a weighted mesh's deform keys have one offset per bone influence; "
                                     "the engine takes one per vertex, so the weighted average is used")
                        deform.setdefault(smap[sraw], {})[att_names[smap[sraw]][araw]] = out_keys
            if deform:
                out["deform"] = deform

        ik_out = {}
        for iraw, keys in (a.get("ik") or {}).items():
            where = f"{where0}, IK constraint '{iraw}'"
            times(keys)
            c = ik_map.get(iraw)
            if c is None:
                imp.warn(f"{where}: no such constraint (or it was dropped); its keys are dropped")
                continue
            if any(("bendPositive" in k and bool(k["bendPositive"]) != (c.get("bend", "positive") == "positive")) for k in keys):
                imp.warn(f"{where}: keyed bend direction is not supported; the setup bend is kept")
            pts = increasing([(float(k.get("time", 0)), [float(k.get("mix", 1))], k) for k in keys], imp, where)
            if pts:
                ik_out[c["name"]] = build_keys(pts, curvefn(where), lambda t, v: {"t": num(t, 5), "mix": num(v[0], 5)})
        if ik_out:
            out["ik"] = ik_out

        for kind in ("transform", "path", "physics"):
            if a.get(kind):
                imp.warn(f"{where0}: {kind} constraint timelines are not supported; dropped")
                for tl in a[kind].values():
                    times(tl if isinstance(tl, list) else [])
        if a.get("drawOrder") or a.get("draworder"):
            imp.warn(f"{where0}: draw order keys are not supported; the setup draw order is kept")
            times(a.get("drawOrder") or a.get("draworder"))

        ev_names = NameSet(imp, "event", MAX_EVENT_NAME)
        events = []
        evs = sorted(a.get("events") or [], key=lambda e: float(e.get("time", 0)))
        times(evs)
        seen = {}
        for e in evs:
            if any(k in e for k in ("int", "float", "string", "volume", "balance")):
                imp.warn(f"{where0}: event '{e.get('name')}' carries a payload, which is not supported; only the name is kept")
            raw = str(e.get("name", ""))
            if raw not in seen:
                base = raw[:MAX_EVENT_NAME] or "event"
                if base != raw:
                    imp.warn(f"event name '{raw}' is longer than {MAX_EVENT_NAME} characters; cut to '{base}'")
                seen[raw] = base
            events.append({"t": num(float(e.get("time", 0)), 5), "name": seen[raw]})
        if events:
            out["events"] = events
        out["duration"] = num(tmax, 5)
        anims.append(out)

    skel = {"bones": bones}
    if slots:
        skel["slots"] = slots
    if anims:
        skel["animations"] = anims
    if iks:
        skel["ik"] = iks
    return skel


# --- DragonBones -----------------------------------------------------------------------------------------------

def db_color(c, imp, where):
    if not c:
        return [1, 1, 1, 1]
    if any(float(c.get(k, 0)) for k in ("aO", "rO", "gO", "bO")):
        imp.warn(f"{where}: color offsets are not supported; only the multipliers are kept")
    return [num(float(c.get(k, 100)) / 100.0, 5) for k in ("rM", "gM", "bM", "aM")]


def db_apply(m, x, y):
    return (m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5])


def db_invert(m):
    det = m[0] * m[3] - m[1] * m[2]
    if abs(det) < 1e-12:
        raise ValueError("a bone pose matrix cannot be inverted")
    a, b, c, d = m[3] / det, -m[1] / det, -m[2] / det, m[0] / det
    return [a, b, c, d, -(a * m[4] + c * m[5]), -(b * m[4] + d * m[5])]


def db_curve_fn(imp, where):
    def fn(src, t1, t2, chans):
        c = src.get("curve")
        if isinstance(c, list):
            if len(c) == 4:
                return clean_bezier([float(v) for v in c], imp, where)
            imp.warn(f"{where}: a multi-segment curve is not supported; linear")
            return None
        if src.get("tweenEasing") is None:
            return "stepped"
        if float(src["tweenEasing"]) != 0:
            imp.warn(f"{where}: ease-in/out tweens are not supported; linear")
        return None
    return fn


def convert_dragonbones(doc, o, imp):
    ppm = o["ppm"]
    atlas, tex = o["atlas"], o["tex"]
    ver = str(doc.get("version", ""))
    if ver and not ver.startswith("5."):
        imp.warn(f"DragonBones {ver} is not 5.x; the JSON is read as 5.x and may be wrong")
    arms = doc.get("armature") or []
    if not arms:
        raise ValueError("the file has no armature")
    arm = arms[0]
    if o.get("armature"):
        arm = next((x for x in arms if x.get("name") == o["armature"]), None)
        if arm is None:
            raise ValueError(f"no armature named '{o['armature']}' (have: {', '.join(str(x.get('name')) for x in arms)})")
    for x in arms:
        if x is not arm:
            imp.warn(f"armature '{x.get('name')}' is dropped; one armature is imported (use --armature to choose)")
    if arm.get("type", "Armature") != "Armature":
        imp.warn(f"armature type '{arm['type']}' is not supported; imported as an armature")
    fps = float(arm.get("frameRate") or doc.get("frameRate") or 24)

    bones_in = arm.get("bone") or []
    finish_bones(imp, bones_in)
    by_name = {b["name"]: i for i, b in enumerate(bones_in)}
    # parents before children, the original order otherwise
    order, placed = [], set()
    while len(order) < len(bones_in):
        progress = False
        for i, b in enumerate(bones_in):
            if i in placed:
                continue
            p = b.get("parent")
            if p and p not in by_name:
                raise ValueError(f"bone '{b['name']}' names parent '{p}', which does not exist")
            if not p or by_name[p] in placed:
                order.append(i)
                placed.add(i)
                progress = True
        if not progress:
            raise ValueError("the bone hierarchy has a cycle")
    if bones_in[order[0]].get("parent"):
        raise ValueError("the skeleton has no root bone")
    bnames = NameSet(imp, "bone")
    bmap, bone_by_index, parents, bones = {}, {}, {}, []
    for i in order:
        b = bones_in[i]
        nm = bnames.make(b["name"])
        bmap[b["name"]] = nm
        bone_by_index[i] = nm
        parents[nm] = bmap[b["parent"]] if b.get("parent") else None
        tf = b.get("transform") or {}
        e = {"name": nm}
        if b.get("parent"):
            e["parent"] = bmap[b["parent"]]
        x, y = float(tf.get("x", 0)) / ppm, -float(tf.get("y", 0)) / ppm
        if abs(x) > FLIP_STEP:
            e["x"] = num(x)
        if abs(y) > FLIP_STEP:
            e["y"] = num(y)
        skx, sky = float(tf.get("skX", 0)), float(tf.get("skY", 0))
        if abs(skx - sky) > 1e-6:
            imp.warn(f"bone '{b['name']}' has skew (skX differs from skY), which is not supported; skY is the rotation")
        rot = -sky
        if abs(rot) > FLIP_STEP:
            e["rotation"] = num(rot)
        sx, sy = float(tf.get("scX", 1)), float(tf.get("scY", 1))
        if abs(sx - 1) > FLIP_STEP or abs(sy - 1) > FLIP_STEP:
            e["scale"] = [num(sx), num(sy)]
        if float(b.get("length", 0)) > 0:
            e["length"] = num(float(b["length"]) / ppm)
        for k in ("inheritTranslation", "inheritRotation", "inheritScale", "inheritReflection"):
            if b.get(k) is False:
                imp.warn(f"bone '{b['name']}': '{k}': false is not supported; the bone inherits normally")
        bones.append(e)

    # skin
    skins = arm.get("skin") or []
    skin = next((s for s in skins if s.get("name", "") in ("", "default")), skins[0] if skins else None)
    for s in skins:
        if s is not skin:
            imp.warn(f"skin '{s.get('name')}' is dropped; only one skin is imported")
    skin_slots = {s["name"]: s.get("display") or [] for s in (skin or {}).get("slot") or []}

    slots_in = arm.get("slot") or []
    if len(slots_in) > MAX_SLOTS:
        raise ValueError(f"the skeleton has {len(slots_in)} slots; the engine allows {MAX_SLOTS}")
    snames = NameSet(imp, "slot")
    smap, slots, atts, disp_names, setup_disp = {}, [], {}, {}, {}
    for s in slots_in:
        raw = s["name"]
        nm = snames.make(raw)
        smap[raw] = nm
        if s.get("parent") not in bmap:
            raise ValueError(f"slot '{raw}' names bone '{s.get('parent')}', which does not exist")
        e = {"name": nm, "bone": bmap[s["parent"]]}
        col = db_color(s.get("color"), imp, f"slot '{raw}'")
        if col != [1, 1, 1, 1]:
            e["tint"] = col
        if s.get("blendMode", "normal") != "normal":
            imp.warn(f"slot '{raw}' blends '{s['blendMode']}', which is not supported; drawn normally")
        names = NameSet(imp, "attachment")
        lst, ainfo, dn = [], {}, []
        for di, d in enumerate(skin_slots.get(raw, [])):
            typ = d.get("type", "image")
            what = f"slot '{raw}', display {di}"
            if typ not in ("image", "mesh"):
                imp.warn(f"{what}: a '{typ}' display is not supported; dropped")
                dn.append(None)
                continue
            if len(lst) >= MAX_SLOT_ATTACHMENTS:
                imp.warn(f"slot '{raw}' has more than {MAX_SLOT_ATTACHMENTS} displays; display {di} is dropped")
                dn.append(None)
                continue
            path = d.get("path", d.get("name"))
            reg = atlas.get(path) if atlas is not None else None
            if atlas is not None and reg is None:
                imp.warn(f"{what}: texture '{path}' is not in the atlas; the whole texture is used")
            tf = d.get("transform") or {}
            if typ == "image":
                if reg is None:
                    imp.warn(f"{what}: without an atlas the image size is unknown; a 1 m quad is used")
                w = reg.ow if reg else ppm
                h = reg.oh if reg else ppm
                q = build_quad(names.make(d.get("name")), tex, reg, w, h, float(tf.get("x", 0)), -float(tf.get("y", 0)),
                               -float(tf.get("skY", tf.get("skX", 0))), float(tf.get("scX", 1)), float(tf.get("scY", 1)),
                               ppm, imp, what)
                lst.append(q)
                dn.append(q["name"])
                ainfo[q["name"]] = Att(q["name"])
            else:
                uvs_in = d.get("uvs") or []
                n = len(uvs_in) // 2
                vin = d.get("vertices") or []
                uvs = [uv_in(reg, float(uvs_in[2 * i]), float(uvs_in[2 * i + 1])) for i in range(n)]
                verts, infl_all = [], None
                if d.get("weights"):
                    wts = d["weights"]
                    sp = d.get("slotPose") or [1, 0, 0, 1, 0, 0]
                    poses = {}
                    bp = d.get("bonePose") or []
                    for k in range(0, len(bp) - 6, 7):
                        poses[int(bp[k])] = db_invert([float(v) for v in bp[k + 1:k + 7]])
                    infl_all, iw = [], 0
                    for vi in range(n):
                        cnt = int(wts[iw])
                        iw += 1
                        gx, gy = db_apply(sp, float(vin[2 * vi]), float(vin[2 * vi + 1]))
                        infl, ws = [], []
                        for _ in range(cnt):
                            bi, w = int(wts[iw]), float(wts[iw + 1])
                            iw += 2
                            if bi not in poses or bi not in bone_by_index:
                                raise ValueError(f"{what}: mesh weight names bone {bi} without a bone pose")
                            lx, ly = db_apply(poses[bi], gx, gy)
                            infl.append((bone_by_index[bi], lx / ppm, -ly / ppm, w))
                            ws.append(w)
                        verts.append(infl)
                        infl_all.append(ws)
                else:
                    for vi in range(n):
                        verts.append([(None, float(vin[2 * vi]) / ppm, -float(vin[2 * vi + 1]) / ppm, 1.0)])
                m = finish_mesh(names.make(d.get("name")), tex, uvs, verts, d.get("triangles") or [], imp, what)
                if m is None:
                    dn.append(None)
                    continue
                lst.append(m)
                dn.append(m["name"])
                ainfo[m["name"]] = Att(m["name"], True, n, infl_all)
        di = int(s.get("displayIndex", 0))
        setup = dn[di] if 0 <= di < len(dn) else None
        if lst:
            e["attachments"] = lst
        e["attachment"] = setup
        setup_disp[nm] = di
        disp_names[nm] = dn
        atts[nm] = ainfo
        slots.append(e)

    iks, ik_map = [], {}
    inames = NameSet(imp, "IK constraint")
    for c in arm.get("ik") or []:
        raw = c.get("name", c.get("bone"))
        if len(iks) >= MAX_IK:
            imp.warn(f"IK constraint '{raw}': the engine allows {MAX_IK} constraints; dropped")
            continue
        if c.get("bone") not in bmap or c.get("target") not in bmap:
            imp.warn(f"IK constraint '{raw}': needs a known bone and target; dropped")
            continue
        chain_n = int(c.get("chain", 0))
        if chain_n not in (0, 1):
            imp.warn(f"IK constraint '{raw}': chain {chain_n} is not supported (0 or 1); treated as 1")
        tip = bmap[c["bone"]]
        chain = [tip]
        if chain_n >= 1 and parents.get(tip):
            chain = [parents[tip], tip]
        target = bmap[c["target"]]
        if not valid_ik(imp, raw, chain, target, parents):
            continue
        e = {"name": inames.make(raw), "bones": chain, "target": target}
        if len(chain) == 2:
            # DragonBones' Y-down turns the other way: its positive bend is the engine's negative one.
            e["type"] = "two_bone"
            e["bend"] = "negative" if c.get("bendPositive", True) else "positive"
        else:
            e["type"] = "ccd"
        if float(c.get("weight", 1)) != 1:
            e["mix"] = num(c["weight"], 5)
        ik_map[raw] = e
        iks.append(e)

    anims = []
    anames = NameSet(imp, "animation")
    for an in arm.get("animation") or []:
        if len(anims) >= MAX_ANIMATIONS:
            imp.warn(f"more than {MAX_ANIMATIONS} animations; '{an.get('name')}' and the rest are dropped")
            break
        aname = an.get("name", "")
        where0 = f"animation '{aname}'"
        out = {"name": anames.make(aname)}
        plays = int(an.get("playTimes", 1))
        out["loop"] = plays == 0
        if plays > 1:
            imp.warn(f"{where0}: playTimes {plays} is not supported; it plays once (set playTimes 0 to loop)")
        tmax = float(an.get("duration", 0)) / fps

        def timed(frames):
            """[(t seconds, frame)] from frame durations in frames."""
            t, res = 0.0, []
            for f in frames or []:
                res.append((t / fps, f))
                t += float(f.get("duration", 0))
            return res

        bones_out = {}
        for bt in an.get("bone") or []:
            braw = bt.get("name")
            where = f"{where0}, bone '{braw}'"
            fr = timed(bt.get("frame"))
            if fr:
                tmax = max(tmax, fr[-1][0])
            if braw not in bmap:
                imp.warn(f"{where}: no such bone; its keys are dropped")
                continue
            if any(f.get("tweenRotate") for _, f in fr):
                imp.warn(f"{where}: extra rotation turns (tweenRotate) are not supported; the short way is used")
            cfn = db_curve_fn(imp, where)
            ch = {}
            raw_rot = [-float((f.get("transform") or {}).get("skY", (f.get("transform") or {}).get("skX", 0))) for _, f in fr]
            rots, _ = unwrap(raw_rot) if raw_rot else ([], False)
            pts = increasing([(t, [r], f) for (t, f), r in zip(fr, rots)], imp, where)
            if any(abs(p[1][0]) > FLIP_STEP for p in pts):
                ch["rotate"] = build_keys(pts, cfn, lambda t, v: {"t": num(t, 5), "value": num(v[0], 5)})
            pts = increasing([(t, [float((f.get("transform") or {}).get("x", 0)), -float((f.get("transform") or {}).get("y", 0))], f)
                              for t, f in fr], imp, where)
            if any(abs(p[1][0]) > FLIP_STEP or abs(p[1][1]) > FLIP_STEP for p in pts):
                ch["translate"] = build_keys(pts, cfn, lambda t, v: {"t": num(t, 5), "x": num(v[0] / ppm), "y": num(v[1] / ppm)})
            pts = increasing([(t, [float((f.get("transform") or {}).get("scX", 1)), float((f.get("transform") or {}).get("scY", 1))], f)
                              for t, f in fr], imp, where)
            if any(abs(p[1][0] - 1) > FLIP_STEP or abs(p[1][1] - 1) > FLIP_STEP for p in pts):
                ch["scale"] = build_keys(pts, cfn, lambda t, v: {"t": num(t, 5), "x": num(v[0], 5), "y": num(v[1], 5)})
            if ch:
                bones_out[bmap[braw]] = ch
        if bones_out:
            out["bones"] = bones_out

        slots_out = {}
        for st in an.get("slot") or []:
            sraw = st.get("name")
            where = f"{where0}, slot '{sraw}'"
            fr = timed(st.get("frame"))
            if fr:
                tmax = max(tmax, fr[-1][0])
            if sraw not in smap:
                imp.warn(f"{where}: no such slot; its keys are dropped")
                continue
            sn = smap[sraw]
            ch = {}
            if any("displayIndex" in f for _, f in fr):
                cur = setup_disp[sn]
                keys = []
                for t, _, f in increasing([(t, [], f) for t, f in fr], imp, where):
                    cur = int(f.get("displayIndex", cur))
                    dn = disp_names[sn]
                    keys.append({"t": num(t, 5), "name": dn[cur] if 0 <= cur < len(dn) else None})
                if keys:
                    ch["attachment"] = keys
            if any("color" in f for _, f in fr):
                cur = None
                pts = []
                for p in increasing([(t, [], f) for t, f in fr], imp, where):
                    f = p[2]
                    cur = db_color(f["color"], imp, where) if "color" in f else (cur or [1, 1, 1, 1])
                    pts.append((p[0], list(cur), f))
                ch["color"] = build_keys(pts, db_curve_fn(imp, where), lambda t, v: {"t": num(t, 5), "color": v})
            if ch:
                slots_out[sn] = ch
        if slots_out:
            out["slots"] = slots_out

        deform = {}
        for ft in an.get("ffd") or []:
            sraw, araw = ft.get("slot"), ft.get("name")
            where = f"{where0}, slot '{sraw}', mesh '{araw}'"
            fr = timed(ft.get("frame"))
            if fr:
                tmax = max(tmax, fr[-1][0])
            info = atts.get(smap.get(sraw), {}).get(araw)
            if info is None or not info.mesh:
                imp.warn(f"{where}: no mesh to deform here; its keys are dropped")
                continue
            pts = increasing([(t, [0.0], f) for t, f in fr], imp, where)
            keys = []
            for i, (t, _, f) in enumerate(pts):
                d = {"t": num(t, 5)}
                offs = deform_offsets(info, [float(x) for x in f.get("vertices") or []], int(f.get("offset", 0)), ppm, True)
                if any(offs):
                    d["offsets"] = offs
                if i + 1 < len(pts):
                    cv = db_curve_fn(imp, where)(f, t, pts[i + 1][0], [])
                    if cv is not None:
                        d["curve"] = cv
                keys.append(d)
            if keys:
                if info.infl is not None:
                    imp.warn(f"{where}: a weighted mesh's deform keys have one offset per bone influence; "
                             "the engine takes one per vertex, so the weighted average is used")
                deform.setdefault(smap[sraw], {})[info.name] = keys
        if deform:
            out["deform"] = deform

        ik_out = {}
        for it in an.get("ik") or []:
            iraw = it.get("name")
            where = f"{where0}, IK constraint '{iraw}'"
            fr = timed(it.get("frame"))
            if fr:
                tmax = max(tmax, fr[-1][0])
            c = ik_map.get(iraw)
            if c is None:
                imp.warn(f"{where}: no such constraint (or it was dropped); its keys are dropped")
                continue
            pts = increasing([(t, [float(f.get("weight", 1))], f) for t, f in fr], imp, where)
            if pts:
                ik_out[c["name"]] = build_keys(pts, db_curve_fn(imp, where), lambda t, v: {"t": num(t, 5), "mix": num(v[0], 5)})
        if ik_out:
            out["ik"] = ik_out

        if an.get("zOrder"):
            imp.warn(f"{where0}: z-order keys are not supported; the setup draw order is kept")
        events = []
        for t, f in timed(an.get("frame")):
            tmax = max(tmax, t)
            for ev in f.get("events") or []:
                raw = str(ev.get("name", ""))
                base = raw[:MAX_EVENT_NAME] or "event"
                if base != raw:
                    imp.warn(f"event name '{raw}' is longer than {MAX_EVENT_NAME} characters; cut to '{base}'")
                if any(ev.get(k) for k in ("ints", "floats", "strings", "bone", "slot")):
                    imp.warn(f"{where0}: event '{raw}' carries a payload, which is not supported; only the name is kept")
                events.append({"t": num(t, 5), "name": base})
            if f.get("sound"):
                imp.warn(f"{where0}: sound frames are not supported; dropped")
            if f.get("actions"):
                imp.warn(f"{where0}: action frames are not supported; dropped")
        if events:
            out["events"] = events
        out["duration"] = num(tmax, 5)
        anims.append(out)

    skel = {"bones": bones}
    if slots:
        skel["slots"] = slots
    if anims:
        skel["animations"] = anims
    if iks:
        skel["ik"] = iks
    return skel


# --- CLI -----------------------------------------------------------------------------------------------------------

def run(tool, args):
    imp = Importer(tool)
    src = args.file
    try:
        if args.ppm <= 0:
            raise ValueError("--ppm must be positive")
        with open(src, "rb") as f:
            head = f.read(4)
        if head == b"DBDT":
            raise ValueError("this is a binary DragonBones file (.dbbin); export the JSON skeleton instead")
        if head[:3] == b"\xef\xbb\xbf" or head[:1] in (b"{", b" ", b"\n", b"\r", b"\t"):
            doc = read_json(src)
        else:
            raise ValueError("this is not JSON (a Spine .skel binary?); export the JSON skeleton instead")
        out = args.out or os.path.splitext(src)[0] + ".mcskel2d"
        if tool == "spine" and "bones" not in doc:
            raise ValueError("no \"bones\" array: not a Spine JSON export")
        if tool == "dragonbones" and "armature" not in doc:
            raise ValueError("no \"armature\" array: not a DragonBones JSON skeleton")
        guid = existing_guid(out) or str(uuid.uuid4())
        tex_ref, side, tex_guid = None, None, None
        if args.texture:
            if not os.path.isfile(args.texture):
                raise ValueError(f"--texture: '{args.texture}' does not exist")
            side = args.texture + ".mceasset"
            tex_guid = existing_guid(side) or str(uuid.uuid4())
            tex_ref = f"guid:{tex_guid}"
        atlas = None
        if args.atlas:
            atlas = parse_libgdx_atlas(args.atlas, imp) if tool == "spine" else parse_db_atlas(args.atlas, imp)
        elif args.texture:
            imp.warn("--texture without --atlas: every attachment uses the whole texture")
        if not args.texture:
            imp.warn("no --texture: the attachments draw as solid quads and meshes in their color")
        opts = {"ppm": float(args.ppm), "atlas": atlas, "tex": tex_ref, "armature": getattr(args, "armature", None)}
        skel = (convert_spine if tool == "spine" else convert_dragonbones)(doc, opts, imp)
        name = args.name or os.path.splitext(os.path.basename(out))[0]
        if len(name) > MAX_NAME:
            imp.warn(f"skeleton name '{name}' is longer than {MAX_NAME} characters; cut")
            name = name[:MAX_NAME]
        result = {"format": "mce-skeleton2d", "version": 1, "guid": guid, "name": name}
        result.update(skel)
    except (OSError, ValueError, KeyError, TypeError, IndexError, AttributeError) as e:
        print(f"mce import {tool}: {src}: {type(e).__name__ if not isinstance(e, ValueError) else 'error'}: {e}", file=sys.stderr)
        return 1
    if imp.warnings and args.strict:
        print(f"mce import {tool}: --strict: {len(imp.warnings)} warning(s); nothing written", file=sys.stderr)
        return 1
    if side and not os.path.isfile(side):
        doc_side = {"guid": tex_guid, "importer": "texture", "importer_version": 1,
                    "settings": {"srgb": True, "mips": False}}
        with open(side, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(doc_side, indent=2) + "\n")
        print(f"wrote {side}")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(result, indent=2) + "\n")
    print(f"wrote {out} ({len(result['bones'])} bones, {len(result.get('slots', []))} slots, "
          f"{len(result.get('animations', []))} animations, {len(result.get('ik', []))} IK)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mce import", description="2D skeleton exports into a .mcskel2d")
    sub = ap.add_subparsers(dest="tool", metavar="<tool>", required=True)
    for tool, what, atlas_help in (("spine", "Spine 3.8/4.x JSON export", "the libGDX .atlas"),
                                   ("dragonbones", "DragonBones 5.x JSON skeleton", "the texture atlas _tex.json")):
        s = sub.add_parser(tool, help=f"{what} into a .mcskel2d")
        s.add_argument("file", help="the exported .json")
        s.add_argument("--out", help="the .mcskel2d to write (default: beside the export)")
        s.add_argument("--name", help="the skeleton's name (default: the output file name)")
        s.add_argument("--ppm", type=float, default=100.0, help="pixels per metre (default 100)")
        s.add_argument("--atlas", help=atlas_help + " that places each image")
        s.add_argument("--texture", help="the atlas page PNG the attachments are cut from")
        s.add_argument("--strict", action="store_true", help="a warning is a failure; nothing is written")
        if tool == "dragonbones":
            s.add_argument("--armature", help="the armature to import (default: the first)")
        s.set_defaults(tool_name=tool)
    args = ap.parse_args(argv)
    return run(args.tool_name, args)


if __name__ == "__main__":
    sys.exit(main())
