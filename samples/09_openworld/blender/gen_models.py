#!/usr/bin/env python3
"""Builds the street props and trees of 09_openworld with Blender's Python API and exports them as .glb.

    pip install bpy numpy           (the `bpy` wheel is Blender as a module; or run it inside Blender: blender -b --python gen_models.py)
    python3 samples/09_openworld/blender/gen_models.py

Writes assets/models/props/{tree_oak,tree_pine,bush,lamp_post,hydrant,bench,bin}.glb. One unit is one metre, +Y is up
(the glTF exporter converts from Blender's Z-up), the origin is the middle of the base, rotation and scale are applied, one
mesh per prop. Trees and bushes are real canopies: crowns of alpha-masked leaf cards (cut-out textures from tools/gen_prop_textures.py,
referenced as external files next to the .glb, Textures/*.png, like the Kenney models), smooth tapered bark tubes with a bark colour and
normal map, and soft crown normals so the canopy shades like one volume; the street props are smooth-shaded where the surface is round
(edges sharper than 35 degrees stay hard). Seeded: the same bytes every run. gen_assets.py writes the sidecars and places the props.
"""
import json
import math
import os
import random
import struct

import bpy
import bmesh  # noqa: E402 (bmesh only imports after bpy)
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "assets", "models", "props"))

LEAF_TEXTURES = {  # material -> glTF texture set (files next to the .glb), cut-out and double-sided where it says so
    "bark": {"base": "Textures/bark_color.png", "normal": "Textures/bark_normal.png"},
    "leaf": {"base": "Textures/leaf_cluster.png", "mask": True},
    "leaf_dark": {"base": "Textures/leaf_cluster.png", "mask": True},
    "pine": {"base": "Textures/pine_bough.png", "mask": True},
    "pine_dark": {"base": "Textures/pine_bough.png", "mask": True},
}
COLORS = {"leaf_dark": (0.78, 0.84, 0.76), "pine_dark": (0.85, 0.9, 0.85), "bark": (1.0, 1.0, 1.0), "leaf": (1.0, 1.0, 1.0), "pine": (1.0, 1.0, 1.0), "metal": (0.10, 0.10, 0.12),
          "hydrant": (0.75, 0.08, 0.06), "wood": (0.45, 0.30, 0.16), "bin": (0.16, 0.30, 0.20), "lampglass": (0.95, 0.85, 0.6)}


def material(name):
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = COLORS[name] + (1.0,)
    bsdf.inputs["Roughness"].default_value = 0.35 if name in ("metal", "hydrant") else (0.9 if name in LEAF_TEXTURES else 0.85)
    bsdf.inputs["Metallic"].default_value = 0.4 if name == "metal" else 0.0
    return m


class Prop:
    """One mesh being built: primitives are appended as bmesh geometry, each face tagged with a material slot."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.slots = []
        self.uv = {}            # vertex -> (u, v) for the geometry that has UVs (cards, tubes)
        self.nrm = {}           # vertex -> custom normal
        self.uv_layer = self.bm.loops.layers.uv.new("UVMap")
        self.textured = False   # true: export UVs and custom normals

    def slot(self, mat):
        if mat not in self.slots:
            self.slots.append(mat)
        return self.slots.index(mat)

    def _tag(self, verts, mat):
        i = self.slot(mat)
        faces = {f for v in verts if hasattr(v, "link_faces") for f in v.link_faces}
        for f in faces:
            f.material_index = i

    def cylinder(self, mat, base, top, r0, r1, segments=8, rot=0.0):
        """A (tapered) cylinder along +Z from base to top (Blender Z-up), centred on x=y=0 unless base is offset."""
        res = bmesh.ops.create_cone(self.bm, cap_ends=True, segments=segments, radius1=r0, radius2=r1, depth=top - base)
        verts = res["verts"]
        bmesh.ops.rotate(self.bm, verts=verts, cent=(0, 0, 0), matrix=Matrix.Rotation(rot, 3, "Z"))
        bmesh.ops.translate(self.bm, verts=verts, vec=(0, 0, (base + top) / 2))
        self._tag(verts, mat)
        return verts

    def box(self, mat, centre, size):
        res = bmesh.ops.create_cube(self.bm, size=1.0)
        verts = res["verts"]
        for v in verts:
            v.co = Vector((v.co.x * size[0] + centre[0], v.co.y * size[1] + centre[1], v.co.z * size[2] + centre[2]))
        self._tag(verts, mat)
        return verts

    def blob(self, mat, centre, radius, rng, jitter=0.18, squash=1.0):
        """A rough low-poly ball (an icosphere with every vertex pushed in or out a little)."""
        res = bmesh.ops.create_icosphere(self.bm, subdivisions=2, radius=radius)
        verts = res["verts"]
        for v in verts:
            d = v.co.normalized()
            r = radius * (1.0 + rng.uniform(-jitter, jitter))
            v.co = Vector((d.x * r + centre[0], d.y * r + centre[1], d.z * r * squash + centre[2]))
        self._tag(verts, mat)
        return verts

    def cone(self, mat, base, height, radius, segments=9):
        res = bmesh.ops.create_cone(self.bm, cap_ends=True, segments=segments, radius1=radius, radius2=0.0, depth=height)
        verts = res["verts"]
        bmesh.ops.translate(self.bm, verts=verts, vec=(0, 0, base + height / 2))
        self._tag(verts, mat)
        return verts

    def quad(self, mat, corners, uvs, normals=None):
        """A face from four corners (counter-clockwise seen from the front) with UVs and optional custom vertex normals."""
        vs = [self.bm.verts.new(c) for c in corners]
        f = self.bm.faces.new(vs)
        f.material_index = self.slot(mat)
        for v, uv in zip(vs, uvs):
            self.uv[v] = uv
        if normals:
            for v, n in zip(vs, normals):
                self.nrm[v] = Vector(n).normalized()
        self.textured = True
        return f

    def card(self, mat, centre, right, up, half_w, half_h, normal_hint=None, flip_u=False):
        """A leaf card centred on `centre`, spanned by the unit vectors `right` and `up`; `normal_hint` is the soft shading normal."""
        r, u = Vector(right).normalized() * half_w, Vector(up).normalized() * half_h
        c = Vector(centre)
        # The front face (counter-clockwise) must be the side the soft normal points to; a back face would be lit upside down.
        if normal_hint is not None and r.cross(u).dot(Vector(normal_hint)) < 0:
            r = -r
            flip_u = not flip_u
        corners = [c - r - u, c + r - u, c + r + u, c - r + u]
        uvs = [(1, 0), (0, 0), (0, 1), (1, 1)] if flip_u else [(0, 0), (1, 0), (1, 1), (0, 1)]
        n = normal_hint if normal_hint is not None else (r.cross(u))
        return self.quad(mat, corners, uvs, [n] * 4)

    def tube(self, mat, a, b, r0, r1, segments=12, v_per_m=0.8, sag=0.0):
        """A smooth tapered tube from point a to point b (Blender Z-up), seam-duplicated ring so UV u runs 0..1 around."""
        a, b = Vector(a), Vector(b)
        axis = (b - a)
        length = axis.length
        axis.normalize()
        ref = Vector((1, 0, 0)) if abs(axis.x) < 0.9 else Vector((0, 1, 0))
        t1 = axis.cross(ref).normalized()
        t2 = axis.cross(t1)
        rings = []
        for k in range(2):
            ring = []
            for i in range(segments + 1):
                ang = 2 * math.pi * (i % segments) / segments
                radial = t1 * math.cos(ang) + t2 * math.sin(ang)
                p = (a if k == 0 else b) + radial * (r0 if k == 0 else r1)
                v = self.bm.verts.new(p)
                self.uv[v] = (i / segments, k * length * v_per_m)
                self.nrm[v] = (radial + axis * (r0 - r1) / max(length, 1e-6)).normalized()
                ring.append(v)
            rings.append(ring)
        for i in range(segments):
            f = self.bm.faces.new([rings[0][i], rings[0][i + 1], rings[1][i + 1], rings[1][i]])
            f.material_index = self.slot(mat)
        self.textured = True

    def smooth_by_angle(self, degrees=35.0):
        """Round surfaces smooth, hard edges (sharper than `degrees`) kept: edges are marked, faces are smooth."""
        limit = math.radians(degrees)
        for e in self.bm.edges:
            e.smooth = not (len(e.link_faces) == 2 and e.calc_face_angle(0.0) > limit)
        for f in self.bm.faces:
            f.smooth = True

    def build(self):
        mesh = bpy.data.meshes.new(self.name)
        bmesh.ops.recalc_face_normals(self.bm, faces=[f for f in self.bm.faces if not self.textured])
        self.bm.verts.index_update()
        order = list(self.bm.verts)
        for f in self.bm.faces:
            for loop in f.loops:
                loop[self.uv_layer].uv = self.uv.get(loop.vert, (0.0, 0.0))
        if not self.textured:
            self.smooth_by_angle()
        normals = [self.nrm.get(v, v.normal) for v in order]
        self.bm.to_mesh(mesh)
        self.bm.free()
        for mat in self.slots:
            mesh.materials.append(material(mat))
        if self.textured:
            for p in mesh.polygons:
                p.use_smooth = True
            mesh.normals_split_custom_set_from_vertices(normals)
        obj = bpy.data.objects.new(self.name, mesh)
        obj["textured"] = self.textured
        bpy.context.scene.collection.objects.link(obj)
        return obj


def _unit(rng):
    while True:
        v = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
        if 0.05 < v.length <= 1:
            return v.normalized()


def _crown_cards(p, rng, centre, radii, count, size, dark_inner=0.55):
    """Fills an ellipsoid crown with leaf cards: random orientation, outward-and-up soft normals, darker material inside."""
    centre = Vector(centre)
    for _ in range(count):
        d = _unit(rng)
        depth = rng.random() ** 0.35                       # most cards near the surface, a few inside for volume
        pos = centre + Vector((d.x * radii[0], d.y * radii[1], d.z * radii[2])) * depth
        outward = Vector((d.x / radii[0], d.y / radii[1], d.z / radii[2])).normalized()
        facing = (outward * 0.65 + _unit(rng) * 0.55).normalized()
        up_ref = Vector((0, 0, 1)) if abs(facing.z) < 0.9 else Vector((1, 0, 0))
        right = facing.cross(up_ref).normalized()
        up = right.cross(facing).normalized()
        a = rng.uniform(0, 2 * math.pi)                   # spin the card about its normal
        right, up = right * math.cos(a) + up * math.sin(a), up * math.cos(a) - right * math.sin(a)
        soft = (outward * 0.85 + Vector((0, 0, 0.45))).normalized()
        s = size * rng.uniform(0.8, 1.2)
        p.card("leaf_dark" if depth < dark_inner else "leaf", pos, right, up, s, s, soft, flip_u=rng.random() < 0.5)


def tree_oak():
    rng = random.Random(11)
    p = Prop("tree_oak")
    p.tube("bark", (0, 0, -0.05), (0, 0, 3.4), 0.34, 0.17, 12, 0.7)
    # three limbs from the trunk top into the crown
    for k in range(3):
        a = k * 2.1 + 0.4
        p.tube("bark", (0, 0, 2.9), (math.cos(a) * 0.9, math.sin(a) * 0.9, 4.5), 0.12, 0.05, 8, 0.7)
    _crown_cards(p, rng, (0.0, 0.0, 5.2), (2.5, 2.5, 2.1), 150, 1.05)
    _crown_cards(p, rng, (0.0, 0.0, 5.2), (1.6, 1.6, 1.3), 40, 0.95, dark_inner=2.0)
    return p.build()


def tree_pine():
    rng = random.Random(23)
    p = Prop("tree_pine")
    p.tube("bark", (0, 0, -0.05), (0, 0, 6.4), 0.24, 0.05, 10, 0.7)
    tiers = ((1.5, 2.0, 0.55, 11), (2.5, 1.75, 0.5, 10), (3.5, 1.45, 0.45, 9), (4.5, 1.15, 0.4, 8), (5.4, 0.85, 0.35, 7))
    for ti, (z, reach, droop, n) in enumerate(tiers):
        for k in range(n):
            a = (k + (0.5 if ti % 2 else 0.0)) / n * 2 * math.pi + rng.uniform(-0.12, 0.12)
            out = Vector((math.cos(a), math.sin(a), 0))
            length = reach * rng.uniform(0.9, 1.1)
            # the bough's stem runs from the trunk outward and down; the card's "up" is that stem direction
            stem = (out * math.cos(droop) + Vector((0, 0, -math.sin(droop)))).normalized()
            tangent = Vector((-out.y, out.x, 0))
            centre = Vector((0, 0, z + 0.2)) + stem * (length / 2)
            soft = (Vector((0, 0, 1)) * 0.8 + out * 0.5).normalized()
            p.card("pine", centre, tangent, stem, length * 0.52, length / 2, soft)
            # a second, shorter bough above it so the tier is not a single flat fan
            if k % 2 == 0:
                stem2 = (out * math.cos(droop + 0.35) + Vector((0, 0, -math.sin(droop + 0.35)))).normalized()
                p.card("pine_dark", Vector((0, 0, z + 0.45)) + stem2 * (length * 0.3), tangent, stem2, length * 0.34, length * 0.3,
                       (Vector((0, 0, 1)) * 0.9 + out * 0.3).normalized())
    # the leader: two crossed cards up the top
    for a in (0.0, math.pi / 2):
        right = Vector((math.cos(a), math.sin(a), 0))
        p.card("pine", Vector((0, 0, 6.35)), right, Vector((0, 0, 1)), 0.34, 0.95, Vector((0, 0, 1)))
    return p.build()


def bush():
    rng = random.Random(5)
    p = Prop("bush")
    _crown_cards(p, rng, (0.0, 0.0, 0.5), (0.85, 0.85, 0.5), 38, 0.5, dark_inner=0.5)
    return p.build()


def lamp_post():
    """A 5.5 m pole on a plinth with an arm reaching 0.9 m along +X to the lamp head (the scene puts the glowing head there)."""
    p = Prop("lamp_post")
    p.cylinder("metal", 0.0, 0.35, 0.22, 0.22, 8)
    p.cylinder("metal", 0.35, 5.5, 0.09, 0.07, 8)
    p.box("metal", (0.45, 0.0, 5.45), (0.9, 0.08, 0.08))
    p.box("metal", (0.9, 0.0, 5.38), (0.34, 0.34, 0.06))     # the lamp's mounting plate under the head
    return p.build()


def hydrant():
    p = Prop("hydrant")
    p.cylinder("hydrant", 0.0, 0.7, 0.13, 0.11, 10)
    p.cylinder("hydrant", 0.7, 0.8, 0.15, 0.15, 10)
    p.cylinder("hydrant", 0.8, 0.88, 0.08, 0.04, 10)
    p.box("hydrant", (0.0, 0.0, 0.5), (0.4, 0.1, 0.1))
    return p.build()


def bench():
    """A park bench 1.8 m long (along X), seat 0.45 m high, backrest to -Y; its front faces +Y."""
    p = Prop("bench")
    for k in range(4):
        p.box("wood", (0.0, 0.05 + k * 0.11 - 0.17, 0.45), (1.8, 0.09, 0.05))
    for k in range(3):
        p.box("wood", (0.0, -0.22, 0.62 + k * 0.1), (1.8, 0.04, 0.07))
    for x in (-0.78, 0.78):
        p.box("metal", (x, 0.0, 0.22), (0.06, 0.5, 0.44))
        p.box("metal", (x, -0.22, 0.7), (0.06, 0.04, 0.5))
    return p.build()


def trash_bin():
    p = Prop("bin")
    p.cylinder("bin", 0.0, 0.85, 0.26, 0.3, 10)
    p.cylinder("metal", 0.85, 0.93, 0.32, 0.32, 10)
    return p.build()


def attach_textures(path):
    """Rewrites the exported .glb so the leaf/bark materials reference external texture files (the exporter has no image nodes to read):
    base colour, normal map, cut-out alpha and double-sided for the leaf cards."""
    data = open(path, "rb").read()
    jl = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + jl])
    rest = data[20 + jl:]
    images, textures = [], []

    def tex(uri):
        if uri not in images:
            images.append(uri)
            textures.append({"sampler": 0, "source": len(images) - 1})
        return images.index(uri)

    touched = False
    for m in doc.get("materials", []):
        spec = LEAF_TEXTURES.get(m.get("name"))
        if not spec:
            continue
        touched = True
        m.setdefault("pbrMetallicRoughness", {})["baseColorTexture"] = {"index": tex(spec["base"])}
        if "normal" in spec:
            m["normalTexture"] = {"index": tex(spec["normal"])}
        if spec.get("mask"):
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = 0.5
            m["doubleSided"] = True
    if not touched:
        return
    doc["images"] = [{"uri": u} for u in images]
    doc["textures"] = textures
    doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    out = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(js) + len(rest)) + struct.pack("<I4s", len(js), b"JSON") + js + rest
    open(path, "wb").write(out)


def export(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    path = os.path.join(OUT, obj.name + ".glb")
    textured = bool(obj.get("textured"))
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=True,
                              export_texcoords=textured, export_normals=True, export_materials="EXPORT", export_cameras=False,
                              export_lights=False, export_animations=False, export_skins=False, export_morph=False)
    if textured:
        attach_textures(path)
    print("wrote", path, len(obj.data.polygons), "faces")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for build in (tree_oak, tree_pine, bush, lamp_post, hydrant, bench, trash_bin):
        export(build())
