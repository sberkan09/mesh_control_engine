#!/usr/bin/env python3
"""Rebuilds assets/ of the 09_coop sample (python3 gen_assets.py). The sample is data only."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PREFAB = "b0c90000-0000-4000-8000-000000000001"
INPUT = "b0c90000-0000-4000-8000-000000000002"
SCENE = "b0c90000-0000-4000-8000-000000000003"


def write(rel, doc):
    path = os.path.join(HERE, "assets", rel)
    with open(path, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")


def key(name, scale=None, axis=None):
    b = {"key": name}
    if axis:
        b["axis"] = axis
    if scale:
        b["scale"] = scale
    return b


def input_map():
    # Keyboard players: 1 WASD, 2 arrows, 3 IJKL, 4 TFGH; every player also has a pad (slot N reads pad N).
    keys = [("W", "S", "A", "D", "Space", "Q"), ("Up", "Down", "Left", "Right", "Return", "RShift"),
            ("I", "K", "J", "L", "U", "O"), ("T", "G", "F", "H", "Y", "R")]
    maps = []
    for n, (up, down, left, right, join, leave) in enumerate(keys, 1):
        maps.append({
            "name": "Coop P%d" % n,
            "player": n,
            "actions": [
                {"name": "Move", "type": "axis2d", "bindings": [
                    key(right), key(left, -1), key(up, axis="y"), key(down, -1, "y"),
                    {"gamepad_axis": "LeftX", "deadzone": [0.15, 1]},
                    {"gamepad_axis": "LeftY", "axis": "y", "deadzone": [0.15, 1], "scale": -1}]},
                {"name": "Join", "bindings": [key(join), {"gamepad_button": "Start"}]},
                {"name": "Leave", "bindings": [key(leave), {"gamepad_button": "Back"}]},
            ]})
    write("input/coop.mcinput", {"format": "mce-input", "version": 1, "guid": INPUT, "maps": maps})


def prefab():
    write("prefabs/player.mcprefab", {
        "format": "mce-prefab", "version": 1, "guid": PREFAB, "name": "player",
        "entities": [{"name": "Player", "components": {
            "LocalPlayer": {"speed": 6},
            "LocalTransform": {},
            "Sprite": {"size": [0.9, 0.9], "layer": 2},
            "RigidBody2D": {"type": "Dynamic", "fixed_rotation": True, "gravity_scale": 0},
            "Collider2D": {"shape": {"half": [0.45, 0.45]}, "density": 1, "friction": 0}}}]})


def scene():
    ents = []

    def add(name, comps):
        ents.append({"id": len(ents) + 1, "name": name, "components": comps})
        return len(ents)

    add("Splits", {"SplitScreen": {"layout": 3, "divider": 4}})
    add("Game", {"CoopGame": {"player": "guid:" + PREFAB, "auto_join": 1}})
    tex = add("Minimap texture", {"RenderTexture": {"width": 256, "height": 192, "update_hz": 10}})
    add("Minimap camera", {
        "Camera2DComponent": {"primary": False, "pixel_snap": False, "fit_width": 26, "fit_height": 18,
                              "clear_color": [0.04, 0.05, 0.07, 1]},
        "CameraView": {"target": "$%d" % (tex - 1)},
        "LocalTransform": {}})
    add("Monitor", {
        "Sprite": {"size": [5.2, 3.9], "layer": -1, "tint": [1, 1, 1, 1]},
        "ShowRenderTexture": {"source": "$%d" % (tex - 1)},
        "LocalTransform": {"position": [0, 5.5, 0]}})
    add("Floor", {"Sprite": {"size": [24, 16], "layer": -3, "tint": [0.16, 0.18, 0.2, 1]},
                  "LocalTransform": {}})
    for name, pos, half in (("Wall N", (0, 8.5), (13, 0.5)), ("Wall S", (0, -8.5), (13, 0.5)),
                            ("Wall W", (-12.5, 0), (0.5, 9)), ("Wall E", (12.5, 0), (0.5, 9))):
        add(name, {"RigidBody2D": {}, "Collider2D": {"shape": {"half": list(half)}, "density": 0},
                   "Sprite": {"size": [half[0] * 2, half[1] * 2], "layer": -2, "tint": [0.4, 0.42, 0.46, 1]},
                   "LocalTransform": {"position": [pos[0], pos[1], 0]}})
    for n, (x, y) in enumerate(((-9, 5), (9, 5), (-9, -5), (9, -5)), 1):
        add("Spawn %d" % n, {"PlayerSpawn": {"player": n}, "LocalTransform": {"position": [x, y, 0]}})
    for n in range(1, 5):
        add("Camera P%d" % n, {
            "Camera2DComponent": {"primary": n == 1, "pixel_snap": False, "fit_height": 10,
                                  "clear_color": [0.03, 0.03, 0.04, 1]},
            "PlayerCamera": {"player": n, "offset": [0, 0, 0], "smoothing": 8},
            "CameraView": {"split": n, "enabled": 0},
            "LocalTransform": {}})
    write("scenes/arena.mcscene", {"format": "mce-scene", "version": 1, "guid": SCENE, "name": "arena",
                                   "entities": ents})


input_map()
prefab()
scene()
