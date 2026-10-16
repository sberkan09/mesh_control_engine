#!/usr/bin/env python3
"""Authors Solstice Reach's island through the project's editor MCP server (solstice_reach_mcp).

This is the record of how assets/scenes/main.mcscene was made: an MCP client (this script, run by
an agent) speaks JSON-RPC to `solstice_reach_mcp --project projects/SolsticeReach` over stdio and
issues the same editor commands a person issues in the editor: create an entity with its
components, save, cook, run headless. Every write goes through the editor's EditorScene funnel;
nothing here writes the .mcscene file itself.

    python3 projects/SolsticeReach/tools/author_island_mcp.py \
        --mcp build/linux-gcc-debug/projects/SolsticeReach/solstice_reach_mcp

The island's shape is tools/island.py (the same height field the terrain chunks and the C++
collider use), so every prop below is dropped onto the ground with island.height(). The districts
and the mirror order follow docs/design/solstice-reach.md §2; the walk the bot takes (Waypoint
entities) is the route a first-time player finds: Landing, Terraces, Cistern Garden, Crystal Cut,
Colonnade, Stair of Hours (top, then landing), back along the terraces and up the goat path to
the Vault.
"""

import argparse
import json
import math
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import island  # noqa: E402

ROOT = HERE.parent

# Save identities (SaveKey): one block per kind so the keys never collide.
KEY_PROGRESS, KEY_KEEPER, KEY_MIRROR, KEY_SHARD = 9001, 9002, 9100, 9200


class Mcp:
    def __init__(self, exe, project, transcript):
        self.proc = subprocess.Popen([exe, "--project", str(project)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.next_id = 1
        self.calls = 0
        self.log = open(transcript, "w", encoding="utf-8") if transcript else None
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "solstice-author", "version": "1"}})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def send(self, msg):
        line = json.dumps(msg, separators=(",", ":"))
        if self.log:
            self.log.write(line + "\n")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"solstice_reach_mcp closed the pipe during {method}")
            if self.log:
                self.log.write(line if line.endswith("\n") else line + "\n")
            reply = json.loads(line)
            if reply.get("id") == rid:
                break
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]

    def tool(self, tool_name, **args):
        self.calls += 1
        result = self.request("tools/call", {"name": tool_name, "arguments": args})
        text = result["content"][0]["text"]
        if result.get("isError"):
            raise RuntimeError(f"{tool_name}({json.dumps(args)[:200]}): {text}")
        return json.loads(text) if text.startswith(("{", "[")) else text

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=60)
        if self.log:
            self.log.close()


# ---- geometry helpers ------------------------------------------------------------------------------

def ground(x, z):
    return island.height(x, z)


def yaw_to(x0, z0, x1, z1):
    """The yaw (about +Y, the game's convention: atan2(x, z)) that points +Z from (x0, z0) at (x1, z1)."""
    return math.atan2(x1 - x0, z1 - z0)


def quat_yaw(yaw):
    return [0.0, round(math.sin(yaw * 0.5), 6), 0.0, round(math.cos(yaw * 0.5), 6)]


def r3(v):
    return [round(float(c), 3) for c in v]


def lerp(a, b, t):
    return a + (b - a) * t


def smooth(t):
    return t * t * (3.0 - 2.0 * t)


def ring(cx, cz, radius, count, phase=0.0):
    out = []
    for k in range(count):
        a = phase + 2.0 * math.pi * k / count
        out.append((cx + radius * math.sin(a), cz + radius * math.cos(a), a))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcp", required=True, help="the project's MCP server (solstice_reach_mcp)")
    ap.add_argument("--transcript", help="write every JSON-RPC line here")
    ap.add_argument("--scene", default="scenes/main.mcscene")
    ap.add_argument("--ticks", type=int, default=300, help="run_headless ticks at the end")
    args = ap.parse_args()

    m = Mcp(args.mcp, ROOT, args.transcript)

    names = {c["name"] for c in m.tool("refl_list_components")["components"]}
    for need in ("Terrain3D", "TerrainFoliage3D", "MeshRef", "PointLight3D", "Animator", "BoxCollider3D", "Camera3DComponent", "Environment3D", "Camera2DComponent",
                 "TextLabel", "Scatter", "Glow", "KeeperModel", "Beam", "HudText", "ReachSounds", "Heliostat", "Sunshard",
                 "KeeperSpawn", "Waypoint", "GullPath", "Progress", "SaveKey"):
        if need not in names:
            sys.exit(f"author_island_mcp: solstice_reach_mcp does not know component {need}")
    assets = {a["path"]: a["guid"] for a in m.tool("asset_list")["assets"]}

    def ref(path):
        g = assets[path]
        return g if g.startswith("guid:") else "guid:" + g

    def mesh(name):
        return ref(f"models/{name}.glb")

    # Re-authoring keeps the scene's GUID (SolsticeReach.mceproj names it): open it and clear it.
    if (ROOT / "assets" / args.scene).exists():
        m.tool("scene_open", path=args.scene, force=True)
        m.tool("scene_clear")
    else:
        m.tool("scene_new", name="main", force=True)

    counts = {}

    def create(name, pos, yaw=0.0, scale=None, **components):
        tr = {"position": r3(pos)}
        if yaw:
            tr["rotation"] = quat_yaw(yaw)
        if scale is not None:
            tr["scale"] = r3(scale if isinstance(scale, (list, tuple)) else (scale, scale, scale))
        comps = {"LocalTransform": tr}
        comps.update(components)
        counts[name] = counts.get(name, 0) + 1
        return m.tool("entity_create", name=f"{name} {counts[name]}" if counts[name] > 1 else name, components=comps)["id"]

    def model(name, mesh_name, pos, yaw=0.0, scale=None, tint=None, shadows=1, cull=0.0, **extra):
        ref = {"mesh": mesh(mesh_name)}
        if tint is not None:
            ref["tint"] = list(tint)
        if cull:
            ref["cull_radius"] = cull
        if not shadows:
            ref["cast_shadows"] = 0
        return create(name, pos, yaw, scale, MeshRef=ref, **extra)

    def on_ground(x, z, lift=0.0):
        return (x, ground(x, z) + lift, z)

    def gull(name, pos, radius, height, speed, phase):
        """A gull circling `pos` (the game moves the entity; its model glides)."""
        create(name, pos, MeshRef={"mesh": mesh("gull"), "interpolate": 1},
               Animator={"clip": "glide", "phase": round(phase * 1.7, 3)},
               GullPath={"radius": radius, "height": height, "speed": speed, "phase": phase})

    # ---- the terrain, the sea ------------------------------------------------------------------
    # The island is one Terrain3D painted sand, grass, dry grass and rock (tools/gen_terrain.py); its
    # grass grows from the grass layer. The game's collider is module/terrain.h, from the same formula.
    island_id = create("Island", (0, 0, 0), Terrain3D={"terrain": ref("terrain/island.mcterrain")})
    create("Island grass", (0, 0, 0), ChildOf={"parent": f"${island_id}"},
           TerrainFoliage3D={"mesh": mesh("grass_tuft"), "layer": 1, "density": 0.8, "max_count": 40000, "seed": 11,
                             "scale_min": 0.7, "scale_max": 1.4, "max_slope_deg": 38.0, "wind_amplitude": 0.06,
                             "cull_distance": 70.0})
    model("Sea", "sea", (0, 0, 0), shadows=0)

    # ---- the run: progress, the Keeper's spawn, the bot's route ---------------------------------
    create("Progress", (0, 0, 0), Progress={}, SaveKey={"key": KEY_PROGRESS})
    create("Keeper Spawn", on_ground(0.0, 72.0), KeeperSpawn={"yaw": round(math.pi, 5)}, SaveKey={"key": KEY_KEEPER})

    # ---- on the 3D kit: the camera, the sky, the Keeper's body, the beams, the HUD, the sounds ----
    # The game moves these every tick (present.cpp); the authored transforms are where they start.
    create("Camera", on_ground(0.0, 76.0, 2.5), math.pi,
           Camera3DComponent={"fov_deg": 62.0, "near_z": 0.08, "far_z": 600.0, "mouse_look": 1})
    # The time of day writes the world light; the scene's Environment3D carries the star map (and
    # the evening the editor viewport shows).
    create("Sky", (0, 0, 0), Environment3D={
        "sky_texture": ref("textures/stars_equirect.png"), "sun_direction": [0.89, -0.37, -0.24], "sun_intensity": 3.4,
        "sun_color": [1.0, 0.92, 0.78], "ambient_sky": [0.42, 0.52, 0.72], "ambient_ground": [0.30, 0.28, 0.22],
        "fog_density": 0.002, "fog_color": [0.74, 0.80, 0.92], "sky_zenith": [0.17, 0.36, 0.76],
        "sky_horizon": [0.72, 0.80, 0.92], "sky_ground": [0.24, 0.23, 0.21], "sky_texture_intensity": 0.0,
        "sky_texture_additive": 1, "shadow_cascades": 4, "shadow_distance": 140.0, "shadow_splits": [0.06, 0.18, 0.45, 1.0]})
    create("Keeper", on_ground(0.0, 72.0), math.pi, KeeperModel={},
           MeshRef={"mesh": mesh("keeper"), "interpolate": 1},
           Animator={"clip": "idle"},
           PointLight3D={"color": [1.0, 0.68, 0.35, 1.0], "offset": [0.36, 1.75, 0.25], "intensity": 3.5, "radius": 9.0})
    for index in range(8):  # one beam per heliostat, then the finale's shaft
        create("Beam" if index < 7 else "Finale shaft", (0, 0, 0), Beam={"mirror": index},
               MeshRef={"mesh": mesh("beam"), "cast_shadows": 0, "visible": 0},
               PointLight3D={"color": [1.0, 0.85, 0.55, 1.0], "intensity": 0.0, "radius": 9.0})
    create("Sounds", (0, 0, 0), ReachSounds={"chime": ref("sfx/chime.wav"), "flare": ref("sfx/flare.wav"),
                                             "organ": ref("sfx/organ.wav")})
    # The HUD: a screen camera at one world unit per pixel, the lines sr_hud fills in.
    create("Screen", (0, 0, 0), Camera2DComponent={"zoom": 1.0, "pixels_per_meter": 1.0, "pixel_snap": False})
    font = ref("fonts/ProggyClean.ttf")
    warm = [1.0, 0.9, 0.7, 1.0]
    for kind, line, height, align in (("Status", 0, 20.0, "Left"), ("Status", 1, 20.0, "Left"), ("Journal", 0, 22.0, "Center"),
                                      ("Journal", 1, 22.0, "Center"), ("Prompt", 0, 26.0, "Center"), ("Banner", 0, 56.0, "Center"),
                                      ("Banner", 1, 26.0, "Center"), ("Banner", 2, 26.0, "Center")):
        create(f"HUD {kind.lower()}", (0, 0, 0), HudText={"kind": kind, "line": line},
               TextLabel={"font": font, "text": "", "color": warm, "height": height, "layer": 50, "align": align})

    waypoints = []

    def waypoint(x, z, mirror=False):
        waypoints.append((x, z, mirror))

    lanterns = []

    def lantern(x, z, yaw=0.0, kind="Lantern", intensity=3.0, radius=7.0, color=(1.0, 0.72, 0.42, 1.0)):
        lanterns.append((x, z, yaw, kind, intensity, radius, color))

    shards = []

    def shard(x, z, lift=0.45):
        shards.append((x, z, lift))

    mirrors = {}

    def mirror(index, x, z, face_yaw):
        mirrors[index] = (x, z, face_yaw)

    def wall_ring(cx, cz, radius, count, skip_near=(), phase=0.0):
        """Low dry-stone walls tangent to a circle, leaving gaps where paths arrive (skip_near)."""
        for (x, z, a) in ring(cx, cz, radius, count, phase):
            if any(math.hypot(x - sx, z - sz) < gap for (sx, sz, gap) in skip_near):
                continue
            model("Wall", "wall_low", on_ground(x, z, -0.1), a + math.pi / 2,
                  BoxCollider3D={"half": [2.0, 0.5, 0.3], "offset": [0, 0.5, 0]})

    def stair(x0, z0, x1, z1, step=4.0):
        """Flights of steps along a ramp's centre line, each scaled to the ground's rise under it."""
        length = math.hypot(x1 - x0, z1 - z0)
        n = max(1, int(length // step))
        yaw = yaw_to(x0, z0, x1, z1)
        for k in range(n):
            t0, t1 = k / n, (k + 1) / n
            ax, az = lerp(x0, x1, t0), lerp(z0, z1, t0)
            bx, bz = lerp(x0, x1, t1), lerp(z0, z1, t1)
            rise = ground(bx, bz) - ground(ax, az)
            if rise < 0.6:
                continue
            sy = max(0.25, min(1.6, rise / 3.0))
            model("Stair flight", "stair_flight", (ax, ground(ax, az) - 0.05, az), yaw, (1.0, sy, 1.0), shadows=1)

    def lanterns_along(x0, z0, x1, z1, count, side=1.8, both=False):
        yaw = yaw_to(x0, z0, x1, z1)
        nx, nz = math.cos(yaw), -math.sin(yaw)  # the ramp's right-hand side
        for k in range(count):
            t = (k + 0.5) / count
            cx, cz = lerp(x0, x1, t), lerp(z0, z1, t)
            lantern(cx + nx * side, cz + nz * side, yaw + math.pi)
            if both:
                lantern(cx - nx * side, cz - nz * side, yaw)

    def trees(kind, points, scale=(0.9, 1.25)):
        for i, (x, z) in enumerate(points):
            s = lerp(scale[0], scale[1], ((i * 7919) % 97) / 97.0)
            model(kind.capitalize(), kind, on_ground(x, z, -0.15), (i * 2.399) % (2 * math.pi), s, cull=7.0 * s)

    def rocks(points, scale=(1.4, 3.0)):
        for i, (x, z) in enumerate(points):
            s = lerp(scale[0], scale[1], ((i * 104729) % 89) / 89.0)
            # BoxCollider3D is in the entity's local space (the game scales it by LocalTransform.scale).
            model("Boulder", ["rock_a", "rock_b", "rock_c"][i % 3], on_ground(x, z, -0.4 * s), (i * 1.7) % (2 * math.pi), s,
                  BoxCollider3D={"half": [0.7, 0.6, 0.7], "offset": [0, 0.5, 0]})

    # ---- the Landing (south cove): pier, boats, nets, the first lanterns -------------------------
    model("Pier", "pier", (0.0, 0.3, 98.0), 0.0, BoxCollider3D={"half": [2.0, 0.2, 12.0], "offset": [0, 1.3, 0]})
    model("Boat", "boat", (8.5, -0.3, 103.0), 0.5)
    model("Boat", "boat", (-10.0, -0.3, 98.0), -0.4)
    model("Boat", "boat", (-6.0, 0.9, 84.0), 1.3)  # hauled up on the sand
    lantern(2.2, 109.0, math.pi)
    lantern(-2.2, 109.0, math.pi)
    lantern(2.4, 86.5, math.pi)
    lantern(-2.4, 86.5, math.pi)
    lantern(6.0, 70.0, 0.0)
    lantern(-6.0, 70.0, 0.0)
    rocks([(16.0, 80.0), (-15.0, 82.0), (18.0, 68.0), (-17.0, 66.0), (12.0, 88.0)])
    trees("olive", [(-14.0, 78.0), (14.0, 77.0), (11.0, 66.0), (-12.0, 66.0)])
    shard(4.0, 80.0)
    shard(-8.0, 76.0)
    shard(0.0, 108.0, 1.8)  # the end of the pier
    gull("Gulls over the cove", (0.0, 4.0, 86.0), 22.0, 12.0, 0.35, 0.0)
    gull("Gulls over the cove", (10.0, 4.0, 80.0), 16.0, 17.0, -0.42, 2.0)

    # Landing -> lower terrace (ramp (0,65) -> (8,50)).
    lanterns_along(0.0, 65.0, 8.0, 50.0, 2, both=True)
    waypoint(2.0, 61.0)
    shard(2.0, 61.0)

    # ---- the Terraces: lower terrace (10,44) h9 and the windmill terrace (-28,40) h14 -----------
    wall_ring(10.0, 44.0, 8.6, 11, skip_near=[(8.0, 51.0, 4.5), (3.0, 40.0, 4.5), (17.0, 41.0, 4.5)])
    trees("olive", [(4.0, 46.0), (15.0, 47.0), (12.0, 39.0), (6.0, 40.0), (16.0, 44.0)])
    lantern(12.0, 48.0, 0.0)
    lantern(8.0, 40.0, 0.0)
    shard(10.0, 44.0)
    shard(14.0, 48.0)
    shard(5.0, 41.0)
    waypoint(10.0, 45.0)
    # lower terrace -> windmill terrace (ramp (4,40) -> (-20,43)).
    lanterns_along(4.0, 40.0, -20.0, 43.0, 3)
    waypoint(-8.0, 41.5)
    wall_ring(-28.0, 40.0, 10.6, 14, skip_near=[(-19.0, 43.0, 4.5), (-37.0, 35.0, 4.5), (-22.0, 31.0, 4.5)], phase=0.2)
    model("Windmill", "windmill", on_ground(-33.0, 45.0, -0.1), 0.6, Animator={"speed": 0.55, "clip": "turn"},
          BoxCollider3D={"half": [2.4, 5.0, 2.4], "offset": [0, 5.0, 0]})
    trees("olive", [(-26.0, 47.0), (-18.0, 37.0), (-34.0, 34.0), (-36.0, 42.0), (-30.0, 31.0), (-16.0, 47.0), (-40.0, 40.0)])
    lantern(-30.0, 44.0, 0.0)
    lantern(-24.0, 33.0, 0.0)
    lantern(-33.0, 38.0, 0.0)
    shard(-28.0, 40.0)
    shard(-24.0, 46.0)
    shard(-36.0, 46.0)
    shard(-31.0, 35.0)
    shard(-18.0, 39.0)
    mirror(0, -24.0, 36.0, yaw_to(-24.0, 36.0, 54.0, 22.0))
    waypoint(-24.0 + 1.5, 36.0, mirror=True)

    # ---- the Cistern Garden (54,8) h14 -------------------------------------------------------------
    # back down to the lower terrace, then the ramp (16,41) -> (44,16).
    waypoint(-8.0, 41.5)
    waypoint(12.0, 42.0)
    lanterns_along(16.0, 41.0, 44.0, 16.0, 3, both=True)
    waypoint(30.0, 28.5)
    shard(30.0, 28.5)
    model("Cistern", "cistern", on_ground(54.0, 8.0, -0.05), 0.0,
          BoxCollider3D={"half": [5.0, 0.6, 0.3], "offset": [0, 0.6, 4.7]})
    create("Cistern wall", on_ground(54.0, 8.0), BoxCollider3D={"half": [5.0, 0.6, 0.3], "offset": [0, 0.6, -4.7]})
    create("Cistern wall", on_ground(54.0, 8.0), BoxCollider3D={"half": [0.3, 0.6, 5.0], "offset": [4.7, 0.6, 0]})
    create("Cistern wall", on_ground(54.0, 8.0), BoxCollider3D={"half": [0.3, 0.6, 5.0], "offset": [-4.7, 0.6, 0]})
    trees("cypress", [(44.0, z) for z in (-2.0, 3.0, 8.0, 13.0, 18.0)] + [(64.0, z) for z in (-2.0, 3.0, 8.0, 13.0, 18.0)],
          scale=(0.85, 1.15))
    trees("olive", [(48.0, -6.0), (60.0, -6.0), (62.0, 20.0)])
    for k, (x, z, _a) in enumerate(ring(54.0, 8.0, 7.5, 12, 0.3)):
        create("Firefly", on_ground(x, z, 1.0 + 0.4 * (k % 3)), scale=0.06,
               MeshRef={"mesh": mesh("sunshard"), "tint": [1.0, 1.0, 0.6, 1.0], "cast_shadows": 0, "interpolate": 1},
               PointLight3D={"color": [0.75, 1.0, 0.45, 1.0], "intensity": 0.0, "radius": 2.5},
               Glow={"intensity": 1.4, "flicker": 0.6, "kind": "Firefly"})
    lantern(49.0, 3.0, 0.0)
    lantern(59.0, 3.0, 0.0)
    lantern(49.0, 13.0, 0.0)
    lantern(59.0, 13.0, 0.0)
    for (x, z) in [(50.0, 20.0), (58.0, 20.0), (46.0, 8.0), (62.0, 8.0), (54.0, -4.0), (57.0, 16.0)]:
        shard(x, z)
    mirror(1, 54.0, 22.0, yaw_to(54.0, 22.0, 44.0, -40.0))
    waypoint(46.0, 18.0)
    waypoint(54.0 - 1.5, 22.0, mirror=True)

    # ---- the Crystal Cut (48,-44) h30: up the ramp (53,-4) -> (50,-34) ----------------------------
    lanterns_along(53.0, -4.0, 50.0, -34.0, 3)
    stair(53.0, -4.0, 50.0, -34.0)
    waypoint(53.0, -3.0)
    waypoint(50.0, -33.0)
    shard(50.0, -33.0)
    rocks([(56.0, -40.0), (58.0, -48.0), (52.0, -54.0), (42.0, -54.0), (38.0, -42.0), (46.0, -34.0), (56.0, -52.0), (40.0, -36.0),
           (59.0, -44.0), (54.0, -37.0), (44.0, -56.0), (36.0, -47.0)], scale=(1.3, 2.4))
    rocks([(50.0, -46.0), (45.0, -48.0), (53.0, -43.0)], scale=(0.8, 1.4))
    for (x, z) in [(49.0, -44.0), (51.0, -47.0), (46.0, -46.5), (53.0, -41.0), (47.0, -41.0), (44.0, -50.0), (55.0, -46.0),
                   (50.0, -51.0), (42.0, -45.0), (57.0, -43.0)]:
        shard(x, z, 0.35)
    trees("pine", [(60.0, -36.0), (62.0, -46.0), (36.0, -56.0), (58.0, -56.0)])
    lantern(46.0, -38.0, 0.0)
    lantern(50.0, -50.0, 0.0)
    mirror(2, 44.0, -40.0, yaw_to(44.0, -40.0, 0.0, -62.0))
    waypoint(44.0 + 1.5, -40.0 + 0.4, mirror=True)
    gull("Gulls over the quarry", (48.0, 30.0, -44.0), 18.0, 14.0, 0.3, 1.0)

    # ---- the Colonnade (-4,-62) h42: the ramp (38,-49) -> (10,-62) ---------------------------------
    lanterns_along(38.0, -49.0, 10.0, -62.0, 3)
    waypoint(37.0, -49.5)
    waypoint(12.0, -61.0)
    shard(12.0, -61.0)
    for row_z in (-66.0, -58.0):
        for k, x in enumerate(range(-16, 9, 4)):
            model("Column", "column", on_ground(float(x), row_z, -0.1), 0.0,
                  BoxCollider3D={"half": [0.45, 2.6, 0.45], "offset": [0, 2.5, 0]})
            if k % 2 == 0 and x < 8:
                model("Arch", "arch", on_ground(float(x) + 2.0, row_z, -0.1), math.pi / 2)
    for k, x in enumerate((-14.0, -6.0, 2.0, 10.0)):
        for row_z, phase in ((-68.5, 0.0), (-55.5, 1.3)):
            model("Banner", "banner", on_ground(x, row_z, -0.05), 0.0,
                  Animator={"phase": round(phase + 0.7 * k, 3), "clip": "flutter" if k % 2 == 0 else "gust"})
    lantern(-14.0, -62.0, 0.0)
    lantern(-6.0, -62.0, 0.0)
    lantern(6.0, -62.0, 0.0)
    lantern(14.0, -62.0, 0.0)
    trees("pine", [(-20.0, -70.0), (4.0, -74.0), (16.0, -70.0), (-10.0, -76.0), (20.0, -64.0)])
    for (x, z) in [(-10.0, -62.0), (4.0, -62.0), (-6.0, -68.0), (8.0, -56.0), (-16.0, -56.0)]:
        shard(x, z)
    mirror(3, 0.0, -62.0, yaw_to(0.0, -62.0, -52.0, -42.0))
    waypoint(0.0 + 1.5, -62.0, mirror=True)
    # the Colonnade -> Vault ramp (-10,-49) -> (-8,-30): the way back down from the finale
    stair(-10.0, -49.0, -8.0, -30.0)
    lanterns_along(-10.0, -49.0, -8.0, -30.0, 3, both=True)

    # ---- the Stair of Hours: top (-52,-42) h40, landing (-66,-10) h28, foot (-56,26) h12 -----------
    # Colonnade -> stair top is the ramp (-18,-58) -> (-47,-45).
    lanterns_along(-18.0, -58.0, -47.0, -45.0, 3)
    waypoint(-20.0, -57.0)
    waypoint(-45.0, -46.0)
    shard(-45.0, -46.0)
    mirror(4, -52.0, -42.0, yaw_to(-52.0, -42.0, -66.0, -10.0))
    waypoint(-52.0 + 1.5, -42.0 + 0.4, mirror=True)
    trees("pine", [(-62.0, -46.0), (-56.0, -52.0), (-44.0, -36.0)])
    # the steep flights, twelve lamps each (the 24 lamps of the journal line)
    stair(-65.0, -13.0, -54.0, -37.0)
    lanterns_along(-65.0, -13.0, -54.0, -37.0, 6, side=2.2, both=True)
    waypoint(-55.0, -36.0)
    waypoint(-64.0, -14.0)
    shard(-60.0, -24.0)
    mirror(5, -66.0, -10.0, yaw_to(-66.0, -10.0, -6.0, -20.0))
    waypoint(-66.0 + 1.5, -10.0 + 0.4, mirror=True)
    shard(-69.0, -8.0)
    shard(-63.0, -13.0)
    stair(-57.0, 21.0, -65.0, -6.0)
    lanterns_along(-57.0, 21.0, -65.0, -6.0, 6, side=2.2, both=True)
    waypoint(-65.0, -5.0)
    waypoint(-58.0, 20.0)
    shard(-61.0, 8.0)
    waypoint(-56.0, 26.0)
    shard(-56.0, 26.0)
    shard(-52.0, 23.0)
    rocks([(-68.0, 26.0), (-64.0, 32.0), (-48.0, 20.0), (-72.0, -16.0), (-74.0, -4.0), (-60.0, -46.0)], scale=(1.8, 3.2))
    lantern(-54.0, 30.0, 0.0)
    gull("Gulls over the cliff", (-66.0, 32.0, -20.0), 20.0, 16.0, -0.28, 0.5)

    # ---- back east along the terraces (ramp (-52,28) -> (-36,35)) and up the goat path -----------
    lanterns_along(-52.0, 28.0, -36.0, 35.0, 2)
    waypoint(-50.0, 29.0)
    waypoint(-36.0, 35.0)
    waypoint(-24.0, 34.0)
    # goat path: (-22,32) -> (-35.5,9.5), its landing at (-38,6), then (-40,3) -> (-12,-10), the Vault
    stair(-22.0, 32.0, -35.5, 9.5)
    lanterns_along(-22.0, 32.0, -35.5, 9.5, 4)
    stair(-40.0, 3.0, -12.0, -10.0)
    lanterns_along(-40.0, 3.0, -12.0, -10.0, 4)
    lantern(-38.0, 8.5, 0.0)
    waypoint(-28.0, 22.0)
    waypoint(-38.0, 5.0)
    waypoint(-34.0, 0.2)
    waypoint(-26.0, -3.5)
    waypoint(-13.0, -9.5)
    shard(-37.0, 7.0)
    shard(-26.0, -3.5)
    shard(-30.0, 19.0)

    # ---- the Vault (-6,-20) h50: the hall, sixteen columns, sconces, the dais, the seventh mirror --
    vault_y = ground(-6.0, -20.0) - 0.7  # the rim's top sits flush with the summit plateau
    model("Vault", "vault", (-6.0, vault_y, -20.0), 0.0, shadows=1)
    for k, (x, z, a) in enumerate(ring(-6.0, -20.0, 9.5, 16, 0.0)):
        model("Vault column", "column", (x, vault_y + 0.7, z), 0.0,
              BoxCollider3D={"half": [0.45, 2.6, 0.45], "offset": [0, 2.5, 0]})
        if k % 2 == 1:
            create("Sconce", (x - math.sin(a) * 0.9, vault_y + 0.7, z - math.cos(a) * 0.9),
                   PointLight3D={"color": [1.0, 0.62, 0.3, 1.0], "offset": [0, 3.2, 0], "intensity": 2.4, "radius": 6.0},
                   Glow={"intensity": 2.4, "flicker": 0.35, "kind": "Sconce"})
    model("Dais", "dais", (-6.0, vault_y + 0.7, -20.0), 0.0)
    mirror(6, -6.0, -20.0, 0.0, )
    waypoint(-7.0, -12.0)
    waypoint(-6.0 + 1.6, -20.0 + 0.6, mirror=True)
    shard(-6.0, -28.0)
    shard(-14.0, -22.0)
    shard(2.0, -18.0)
    trees("pine", [(-20.0, -28.0), (8.0, -30.0), (-18.0, -12.0), (8.0, -10.0), (0.0, -36.0)])
    lantern(-10.0, -8.5, 0.0)
    lantern(-3.0, -9.0, 0.0)

    # ---- the seven heliostats ---------------------------------------------------------------------
    mirror_ids = {}
    for index in range(7):
        x, z, face = mirrors[index]
        y = ground(x, z)
        base = create(f"Heliostat {index + 1}", (x, y, z), face,
                      MeshRef={"mesh": mesh("mirror_base")},
                      BoxCollider3D={"half": [0.6, 0.9, 0.6], "offset": [0, 0.9, 0]},
                      SaveKey={"key": KEY_MIRROR + index})
        dish = create(f"Heliostat {index + 1} dish", (x, y + 1.9, z), face, MeshRef={"mesh": mesh("mirror_dish")})
        mirror_ids[index] = (base, dish)
    for index in range(7):
        base, dish = mirror_ids[index]
        nxt = mirror_ids[index + 1][0] if index + 1 < 7 else None
        value = {"dish": f"${dish}", "index": index}
        if nxt is not None:
            value["next"] = f"${nxt}"
        m.tool("component_add", entity=base, component="Heliostat", value=value)

    # ---- lanterns, shards, waypoints -------------------------------------------------------------
    for (x, z, yaw, kind, intensity, radius, color) in lanterns:
        model("Lantern", "lantern", on_ground(x, z, -0.05), yaw, shadows=0,
              PointLight3D={"color": list(color), "offset": [0, 2.05, 0.42], "intensity": intensity, "radius": radius},
              Glow={"intensity": intensity, "flicker": 0.25, "kind": kind})
    for k, (x, z, lift) in enumerate(shards):
        model("Sunshard", "sunshard", on_ground(x, z, lift), (k * 0.9) % (2 * math.pi), 1.0, shadows=0,
              Sunshard={}, SaveKey={"key": KEY_SHARD + k})
    for order, (x, z, is_mirror) in enumerate(waypoints):
        create("Waypoint", on_ground(x, z), Waypoint={"order": order, "mirror": 1 if is_mirror else 0})

    # ---- scatter: grass, flowers, trees and stones in the thousands -------------------------------
    def scatter(name, pos, half, density, meshes, seed, **kw):
        s = {"mesh": mesh(meshes[0]), "half_extent": list(half), "density": density, "seed": seed}
        if len(meshes) > 1:
            s["mesh_b"] = mesh(meshes[1])
        if len(meshes) > 2:
            s["mesh_c"] = mesh(meshes[2])
        s.update(kw)
        create(name, (pos[0], ground(*pos), pos[1]), Scatter=s)

    # Grass grows from the terrain's grass layer (Island grass, above).
    scatter("Flowers terraces", (-8.0, 42.0), (36.0, 14.0), 0.09, ["flower"], 21, min_scale=0.8, max_scale=1.3,
            max_slope_deg=30.0, min_height=4.0)
    scatter("Flowers garden", (54.0, 8.0), (16.0, 16.0), 0.1, ["flower"], 22, min_scale=0.8, max_scale=1.3,
            max_slope_deg=30.0, min_height=4.0)
    scatter("Flowers summit", (-6.0, -34.0), (26.0, 14.0), 0.06, ["flower"], 23, min_scale=0.8, max_scale=1.3,
            max_slope_deg=30.0, min_height=4.0, max_height=49.0)
    scatter("Olive groves", (-8.0, 48.0), (40.0, 18.0), 0.012, ["olive"], 31, min_scale=0.7, max_scale=1.2,
            max_slope_deg=28.0, min_height=4.0, max_height=20.0)
    scatter("Pines north", (-10.0, -60.0), (60.0, 24.0), 0.009, ["pine"], 32, min_scale=0.8, max_scale=1.4,
            max_slope_deg=32.0, min_height=28.0, max_height=49.0)
    scatter("Pines west", (-62.0, -20.0), (20.0, 30.0), 0.008, ["pine"], 33, min_scale=0.8, max_scale=1.3,
            max_slope_deg=32.0, min_height=20.0)
    scatter("Stones", (0.0, 0.0), (96.0, 96.0), 0.014, ["rock_a", "rock_b", "rock_c"], 41, min_scale=0.3, max_scale=1.1,
            max_slope_deg=70.0, min_height=0.4, max_height=49.0, align_to_ground=1)
    scatter("Shore stones", (0.0, 0.0), (96.0, 96.0), 0.02, ["rock_b", "rock_c"], 42, min_scale=0.2, max_scale=0.7,
            max_slope_deg=80.0, min_height=-0.5, max_height=1.5, align_to_ground=1)

    saved = m.tool("scene_save", path=args.scene, force=True)
    cook = m.tool("cook_run")
    run = m.tool("run_headless", ticks=args.ticks, log_lines=0)
    m.close()
    print(f"author_island_mcp: {m.calls} tool calls; {saved['entities']} entities saved to {saved['path']} "
          f"(guid {saved['guid']}); {len(mirrors)} mirrors, {len(shards)} shards, {len(lanterns)} lanterns, "
          f"{len(waypoints)} waypoints; cook ok={cook['ok']} cooked={cook['cooked']} errors={cook['errors']}; "
          f"run_headless {json.dumps(run)[:400]}")
    return 0 if cook["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
