#!/usr/bin/env python3
"""Builds the street props and trees of 09_openworld with Blender's Python API and exports them as .glb.

    pip install bpy numpy           (the `bpy` wheel is Blender as a module; or run it inside Blender: blender -b --python gen_models.py)
    python3 samples/09_openworld/blender/gen_models.py

Writes assets/models/props/{tree_oak,tree_pine,bush,lamp_post,hydrant,bench,bin}.glb. One unit is one metre, +Y is up
(the glTF exporter converts from Blender's Z-up), the origin is the middle of the base, rotation and scale are applied, one
UV-less flat-shaded mesh with plain colour materials per prop (they are tinted per entity in the scene). Seeded: the same
bytes every run. gen_assets.py writes the sidecars and places the props.
"""
import math
import os
import random

import bpy
import bmesh  # noqa: E402 (bmesh only imports after bpy)
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "assets", "models", "props"))

COLORS = {"bark": (0.30, 0.20, 0.12), "leaf": (0.22, 0.46, 0.16), "pine": (0.10, 0.30, 0.14), "metal": (0.10, 0.10, 0.12),
          "hydrant": (0.75, 0.08, 0.06), "wood": (0.45, 0.30, 0.16), "bin": (0.16, 0.30, 0.20), "lampglass": (0.95, 0.85, 0.6)}


def material(name):
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = COLORS[name] + (1.0,)
    bsdf.inputs["Roughness"].default_value = 0.35 if name in ("metal", "hydrant") else 0.85
    bsdf.inputs["Metallic"].default_value = 0.4 if name == "metal" else 0.0
    return m


class Prop:
    """One mesh being built: primitives are appended as bmesh geometry, each face tagged with a material slot."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.slots = []

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

    def build(self):
        mesh = bpy.data.meshes.new(self.name)
        bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces)
        self.bm.to_mesh(mesh)
        self.bm.free()
        for mat in self.slots:
            mesh.materials.append(material(mat))
        for p in mesh.polygons:
            p.use_smooth = False
        obj = bpy.data.objects.new(self.name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        return obj


def tree_oak():
    rng = random.Random(11)
    p = Prop("tree_oak")
    p.cylinder("bark", 0.0, 3.4, 0.30, 0.17, 8)
    # three limbs into the crown
    for k in range(3):
        a = k * 2.1 + 0.4
        p.box("bark", (math.cos(a) * 0.35, math.sin(a) * 0.35, 3.3), (0.14, 0.14, 0.9))
    for centre, r in (((0.0, 0.0, 5.3), 1.9), ((1.2, 0.5, 4.6), 1.35), ((-1.1, 0.6, 4.7), 1.4), ((0.2, -1.2, 4.5), 1.3),
                      ((0.5, 0.3, 6.5), 1.2), ((-0.7, -0.5, 6.0), 1.1)):
        p.blob("leaf", centre, r, rng, 0.16, 0.9)
    return p.build()


def tree_pine():
    rng = random.Random(23)
    p = Prop("tree_pine")
    p.cylinder("bark", 0.0, 2.2, 0.22, 0.12, 7)
    for i, (base, h, r) in enumerate(((1.4, 2.6, 1.7), (2.8, 2.4, 1.35), (4.1, 2.2, 1.0), (5.3, 2.0, 0.65))):
        verts = p.cone("pine", base, h, r, 9)
        for v in verts:                      # a little irregularity so the rings are not perfect cones
            v.co.x += rng.uniform(-0.05, 0.05)
            v.co.y += rng.uniform(-0.05, 0.05)
    return p.build()


def bush():
    rng = random.Random(5)
    p = Prop("bush")
    for centre, r in (((0.0, 0.0, 0.45), 0.55), ((0.55, 0.15, 0.35), 0.42), ((-0.5, -0.1, 0.38), 0.45)):
        p.blob("leaf", centre, r, rng, 0.2, 0.85)
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


def export(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    path = os.path.join(OUT, obj.name + ".glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=True,
                              export_texcoords=False, export_normals=True, export_materials="EXPORT", export_cameras=False,
                              export_lights=False, export_animations=False, export_skins=False, export_morph=False)
    print("wrote", path, len(obj.data.polygons), "faces")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for build in (tree_oak, tree_pine, bush, lamp_post, hydrant, bench, trash_bin):
        export(build())
