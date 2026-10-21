#!/usr/bin/env python3
"""Authors Undertow's scenes through the project's editor MCP server (undertow_mcp, editor-mcp.md).

This is the record of how assets/scenes/main.mcscene and attract.mcscene were made, and the stress
test of the MCP surface the game was built as: an MCP client (this script, run by an agent) speaks
JSON-RPC to `undertow_mcp --project projects/Undertow` over stdio and issues the same editor commands
a person issues in undertow_editor. Every write goes through the editor's EditorScene funnel; the
server's scene_save writes the files. The art and sounds come from tools/gen_art.py and
tools/gen_sfx.py; their sidecars are written by the server's cook_run {"adopt": true}; the particle
presets (.mcpart) are written here (there is no MCP tool for them, see the findings report).

    python3 projects/Undertow/tools/author_level_mcp.py --mcp build/linux-gcc-debug/projects/Undertow/undertow_mcp
    python3 projects/Undertow/tools/author_level_mcp.py --mcp ... --transcript /tmp/undertow_mcp.jsonl --friction /tmp/friction.json

The level is carved from rock as rectangles (y up, 1 cell = 1 m; `carve` below is the designer's
sketch) and printed as ASCII at the end for the record.
"""

import argparse
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
W, H = 100, 56

# ---- the level sketch --------------------------------------------------------------------------
# Air rectangles (x0, y0, x1, y1), inclusive cells. Everything else is rock.
ROOMS = {
    "hub": (30, 44, 70, 53),            # the surface pool with the dock
    "hub_to_z1": (28, 45, 30, 48),
    "z1": (4, 38, 28, 50),              # zone 1: the first cavern, a down-current, core 1
    "z1_shaft": (7, 24, 10, 38),        # down to zone 3, through the timed gate
    "z1_switch": (11, 32, 13, 34),      # alcove: switch a (outside the gate)
    "z3_switch": (11, 21, 13, 23),      # alcove: switch a2 (inside, so nobody is locked in)
    "z3": (4, 6, 28, 24),               # zone 3: mines, core 3
    "z3_to_z4": (28, 10, 32, 13),
    "z4": (32, 4, 66, 22),              # zone 4: cross currents, mines, core 4
    "updraft": (48, 22, 52, 44),        # the central shaft: a strong up-current back to the hub
    "hub_to_z2": (70, 46, 72, 49),
    "z2_shaft": (72, 30, 78, 46),       # zone 2: crushers in the shaft
    "relay": (66, 20, 90, 30),          # the relay chamber: checkpoint, core 2, switch b
    "z4_to_relay": (62, 20, 68, 23),
    "z5_shaft": (84, 16, 87, 20),       # down to zone 5, through gate b
    "z5": (70, 4, 96, 16),              # zone 5: the vertical crusher, mines, core 5
}

DOCK = (50.0, 45.0)
DRONE_START = (44.0, 48.0)
RELAY = (76.0, 21.0)
CORES = [(14.0, 41.0), (80.0, 22.0), (22.0, 9.0), (60.0, 7.0), (93.0, 6.0)]
CRYSTALS = [(6, 49), (24, 49), (34, 52), (66, 52), (6, 23), (26, 20), (34, 21), (46, 5), (64, 21), (88, 29), (72, 15), (94, 15), (16, 7)]
MINES = [(14.0, 15.0), (20.0, 18.0), (10.0, 11.0), (44.0, 18.0), (58.0, 12.0), (36.0, 16.0), (76.0, 8.0), (90.0, 12.0)]
# Currents: centre, half extents, force (newtons per unit mass).
CURRENTS = [((16.0, 44.0), (3.0, 5.0), (0.0, -3.0)), ((40.0, 8.0), (6.0, 1.5), (7.0, 0.0)), ((54.0, 14.0), (6.0, 1.5), (-7.0, 0.0)),
            ((50.0, 33.0), (2.0, 10.0), (0.0, 9.0))]
# Crushers: centre, extent, period, phase, size.
CRUSHERS = [((74.5, 40.0), (2.5, 0.0), 3.2, 0.0, (3.0, 1.5)), ((75.5, 35.0), (2.5, 0.0), 3.2, 3.14, (3.0, 1.5)),
            ((80.0, 10.0), (0.0, 3.0), 2.6, 0.0, (3.0, 1.5))]
# Gates and switches: horizontal gates across shafts; a switch opens its channel for a while.
GATES = [((8.5, 28.0), (4.0, 0.5), 0), ((85.5, 18.0), (4.0, 0.5), 1)]
SWITCHES = [((12.0, 33.0), 0, 9.0), ((12.0, 22.0), 0, 9.0), ((70.0, 21.0), 1, 12.0)]
BUBBLES = [(10, 40), (50, 46), (60, 10), (80, 24), (20, 12)]

# The attract scene's autopilot: fetch core 1 and dock it.
ATTRACT_ROUTE = [(36.0, 47.0), (27.0, 46.5), (20.0, 43.0), (15.2, 39.7), (22.0, 46.5), (30.0, 46.5), (44.0, 47.0), (50.0, 46.8)]
ATTRACT_TETHER_AT = 3  # waypoint index where the tether is thrown


def build_grid():
    grid = [["#"] * W for _ in range(H)]
    for (x0, y0, x1, y1) in ROOMS.values():
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                grid[y][x] = "."
    return grid


def rock(grid, x, y):
    return x < 0 or y < 0 or x >= W or y >= H or grid[y][x] == "#"


def rectangles(grid):
    """Greedy cover of the rock cells by rectangles: one static collider each."""
    used = set()
    out = []
    for y in range(H):
        for x in range(W):
            if grid[y][x] != "#" or (x, y) in used:
                continue
            x1 = x
            while x1 + 1 < W and grid[y][x1 + 1] == "#" and (x1 + 1, y) not in used:
                x1 += 1
            y1 = y
            while y1 + 1 < H and all(grid[y1 + 1][i] == "#" and (i, y1 + 1) not in used for i in range(x, x1 + 1)):
                y1 += 1
            for j in range(y, y1 + 1):
                for i in range(x, x1 + 1):
                    used.add((i, j))
            out.append((x, y, x1, y1))
    return out


def boundary_tiles(grid):
    """Rock cells that touch air (8-neighbourhood): these get a lit sprite each."""
    out = []
    for y in range(H):
        for x in range(W):
            if grid[y][x] != "#":
                continue
            if any(not rock(grid, x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1) if dx or dy):
                out.append((x, y))
    return out


def interior_rectangles(grid, boundary):
    """Greedy rectangles of rock cells that are not boundary tiles: one dark fill sprite each."""
    bset = set(boundary)
    used = set()
    out = []
    for y in range(H):
        for x in range(W):
            if grid[y][x] != "#" or (x, y) in bset or (x, y) in used:
                continue

            def ok(i, j):
                return 0 <= i < W and 0 <= j < H and grid[j][i] == "#" and (i, j) not in bset and (i, j) not in used

            x1 = x
            while ok(x1 + 1, y):
                x1 += 1
            y1 = y
            while all(ok(i, y1 + 1) for i in range(x, x1 + 1)):
                y1 += 1
            for j in range(y, y1 + 1):
                for i in range(x, x1 + 1):
                    used.add((i, j))
            out.append((x, y, x1, y1))
    return out


def ascii_map(grid):
    g = [row[:] for row in grid]

    def put(x, y, ch):
        x, y = int(x), int(y)
        if 0 <= x < W and 0 <= y < H:
            g[y][x] = ch

    put(*DOCK, "D")
    put(*RELAY, "R")
    put(*DRONE_START, "P")
    for i, c in enumerate(CORES):
        put(c[0], c[1], str(i + 1))
    for c in CRYSTALS:
        put(c[0], c[1], "*")
    for m in MINES:
        put(m[0], m[1], "M")
    for (cx, cy), (hx, hy), (fx, fy) in CURRENTS:
        ch = ">" if fx > 0 else "<" if fx < 0 else "^" if fy > 0 else "v"
        for y in range(int(cy - hy), int(cy + hy)):
            for x in range(int(cx - hx), int(cx + hx)):
                if (x + y) % 3 == 0 and g[y][x] == ".":
                    put(x, y, ch)
    for (cx, cy), ext, _, _, size in CRUSHERS:
        put(cx, cy, "X" if ext[0] else "Y")
    for (cx, cy), (sw, sh), ch in GATES:
        for x in range(int(cx - sw / 2), int(cx + sw / 2)):
            put(x, cy, "=")
    for (sx, sy), ch, _ in SWITCHES:
        put(sx, sy, "a" if ch == 0 else "b")
    return "\n".join("".join(row) for row in reversed(g))


# ---- the MCP client ----------------------------------------------------------------------------

class Mcp:
    def __init__(self, exe, project, transcript, friction):
        self.proc = subprocess.Popen([exe, "--project", str(project)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.next_id = 1
        self.calls = 0
        self.errors = []
        self.timings = {}
        self.friction = friction
        self.log = open(transcript, "w", encoding="utf-8") if transcript else None
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "undertow-author", "version": "1"}})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.tools = {t["name"] for t in self.request("tools/list")["tools"]}

    def has_tool(self, name):
        return name in self.tools

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
                raise RuntimeError(f"undertow_mcp closed the pipe during {method}")
            if self.log:
                self.log.write(line if line.endswith("\n") else line + "\n")
            reply = json.loads(line)
            if reply.get("id") == rid:
                break  # notifications (progress) are skipped
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]

    def tool(self, tool_name, _expect_error=False, **args):
        self.calls += 1
        t0 = time.perf_counter()
        result = self.request("tools/call", {"name": tool_name, "arguments": args})
        dt = time.perf_counter() - t0
        t = self.timings.setdefault(tool_name, [0, 0.0, 0.0])
        t[0] += 1
        t[1] += dt
        t[2] = max(t[2], dt)
        text = result["content"][0]["text"]
        if result.get("isError"):
            if _expect_error:
                return {"error": text}
            self.errors.append({"tool": tool_name, "args": args, "error": text})
            raise RuntimeError(f"{tool_name}({json.dumps(args)[:200]}): {text}")
        if _expect_error:
            raise RuntimeError(f"{tool_name} was expected to fail: {text[:200]}")
        return json.loads(text) if text.startswith(("{", "[")) else text

    def resource(self, uri):
        return self.request("resources/read", {"uri": uri})

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=30)
        if self.log:
            self.log.close()


def note(friction, kind, text):
    friction.append({"kind": kind, "text": text})
    print(f"  [{kind}] {text}")


# ---- authoring ---------------------------------------------------------------------------------

def write_presets(effects_dir, tex, guid_for):
    """Particle presets: JSON assets the cook imports. There is no MCP tool for them."""
    def preset(name, **kw):
        base = {"format": "mce-particles", "version": 1, "guid": guid_for(name), "name": name, "texture": tex("dot"),
                "rate": 0, "burst": 0, "max_particles": 256, "duration": 0, "lifetime": [0.3, 0.6], "speed": [1, 2],
                "direction": 90, "spread": 20, "gravity": [0, 0], "drag": 1.0, "size": [0.3, 0.05], "spin": [0, 0],
                "area": [0, 0], "color_start": [1, 1, 1, 1], "color_end": [1, 1, 1, 0], "blend": "additive"}
        base.update(kw)
        (effects_dir / f"{name}.mcpart").write_text(json.dumps(base, indent=2) + "\n", encoding="utf-8", newline="\n")
        return base["guid"]

    out = {}
    for name, direction in (("thrust_right", 0), ("thrust_left", 180), ("thrust_up", 90), ("thrust_down", 270)):
        out[name] = preset(name, rate=90, direction=direction, spread=16, speed=[2.5, 4.5], lifetime=[0.25, 0.5],
                           size=[0.5, 0.08], drag=2.5, color_start=[0.75, 0.95, 1, 0.9], color_end=[0.2, 0.4, 1, 0])
    out["boom"] = preset("boom", speed=[2, 8], lifetime=[0.35, 0.9], spread=180, size=[0.55, 0.05], drag=2.0,
                         color_start=[1, 0.8, 0.4, 1], color_end=[1, 0.2, 0.05, 0], max_particles=512)
    out["spark"] = preset("spark", texture=tex("spark"), speed=[3, 7], lifetime=[0.15, 0.4], spread=180, size=[0.14, 0.02],
                          drag=3.0, gravity=[0, -6], color_start=[1, 1, 0.8, 1], color_end=[1, 0.6, 0.2, 0])
    for name, direction in (("current_right", 0), ("current_left", 180), ("current_up", 90), ("current_down", 270)):
        out[name] = preset(name, texture=tex("bubble"), rate=10, direction=direction, spread=8, speed=[1.6, 2.6],
                           lifetime=[1.4, 2.4], size=[0.1, 0.16], drag=0, area=[1.4, 1.4], blend="alpha",
                           color_start=[0.6, 0.9, 1, 0.0], color_end=[0.6, 0.9, 1, 0.55], max_particles=96)
    out["bubbles"] = preset("bubbles", texture=tex("bubble"), rate=2.5, direction=90, spread=12, speed=[0.3, 0.7],
                            lifetime=[3, 5], size=[0.08, 0.2], area=[2.5, 0.5], blend="alpha",
                            color_start=[0.7, 0.9, 1, 0.5], color_end=[0.7, 0.9, 1, 0], max_particles=64)
    return out


def author(m, friction, scene_path, attract):
    grid = build_grid()
    assets = {a["path"]: a["guid"] for a in m.tool("asset_list")["assets"]}

    def guid(path):
        if path not in assets:
            raise RuntimeError(f"asset not registered: {path}")
        return assets[path]

    tex = lambda name: guid(f"textures/{name}.png")  # noqa: E731
    sfx = lambda name: guid(f"sfx/{name}.wav")  # noqa: E731
    fx = lambda name: guid(f"effects/{name}.mcpart")  # noqa: E731
    font = guid("fonts/ProggyClean.ttf")

    # Re-authoring keeps the scene's GUID (the .mceproj names it): open it and clear it.
    if (ROOT / "assets" / scene_path).exists():
        m.tool("scene_open", path=scene_path, force=True)
        if m.has_tool("scene_clear"):
            m.tool("scene_clear")  # one call, one undo step
        else:
            ents = m.tool("scene_list_entities")["entities"]
            for e in ents:
                m.tool("entity_delete", entity=e["id"])
            note(friction, "gap", f"clearing {scene_path} took {len(ents)} entity_delete calls: no scene_clear / bulk delete tool")
    else:
        m.tool("scene_new", name=pathlib.Path(scene_path).stem, force=True)

    def create(name, pos, **components):
        comps = {"LocalTransform": {"position": [pos[0], pos[1], 0]}}
        comps.update(components)
        return m.tool("entity_create", name=name, components=comps)["id"]

    def place(proto, pos):
        copy = m.tool("entity_duplicate", entity=proto)["ids"][0]
        m.tool("component_set", entity=copy, component="LocalTransform", path="position", value=[pos[0], pos[1], 0])
        return copy

    def stamp(name, positions, **components):
        if not positions:
            return []
        first = create(name, positions[0], **components)
        return [first] + [place(first, p) for p in positions[1:]]

    world_body = {"type": "Static", "layer": 0}
    sensor = {"type": "Static", "layer": 3, "mask": 2}       # sees the drone only
    pad_sensor = {"type": "Static", "layer": 3, "mask": 6}   # sees the drone and the pods

    # ---- camera, backdrop, lighting, HUD --------------------------------------------------------
    ppm = 40
    half_w, half_h = 1280 / ppm / 2, 720 / ppm / 2
    create("Camera", DRONE_START,
           Camera2DComponent={"pixels_per_meter": ppm, "clear_color": [0.02, 0.03, 0.05, 1]},
           Post2DComponent={"vignette": True, "vignette_intensity": 0.45, "vignette_radius": 0.85, "vignette_softness": 0.7,
                            "color_grade": True, "lut": tex("lut_teal"), "lut_strength": 0.8, "chromatic_aberration": False,
                            "aberration": 0.002},
           CameraRig={"min": [half_w, half_h], "max": [W - half_w, H - half_h], "lookahead": 0.28, "smoothing": 0.1, "win_zoom": 1.3},
           AudioListener={})
    create("Cave Far", (0, 0), Sprite={"texture": tex("cave_far"), "size": [64, 40], "layer": -40},
           Parallax={"factor": [0.1, 0.1]})
    create("Ambient", (50, 28), Light2D={"kind": "Ambient", "color": [0.5, 0.62, 0.78, 1], "intensity": 0.42})
    create("Director", (50, 28), RunStats={},
           AudioSource={"clip": sfx("music"), "gain_db": -9, "bus": 1, "loop": True, "play_on_start": True, "priority": 255})
    create("Flash", (0, 0), Sprite={"texture": tex("white"), "size": [2 * half_w + 2, 2 * half_h + 2], "layer": 90,
                                    "tint": [1, 1, 1, 0]},
           Parallax={"factor": [0, 0]}, ScreenFlash={"max_alpha": 0.5})
    create("HUD Status", (-half_w + 0.5, half_h - 0.6),
           TextLabel={"font": font, "text": "HULL ###  ENERGY ||||||||||  CORES 0/5  0:00", "color": [0.85, 0.95, 1, 1],
                      "height": 0.5, "layer": 95, "align": "Left"},
           Parallax={"factor": [0, 0]}, HudText={"kind": "Status"})
    create("HUD Timer", (half_w - 0.5, half_h - 0.6),
           TextLabel={"font": font, "text": "", "color": [1, 0.6, 0.5, 1], "height": 0.5, "layer": 95, "align": "Right"},
           Parallax={"factor": [0, 0]}, HudText={"kind": "Timer"})
    create("HUD Banner", (0, 4.2),
           TextLabel={"font": font, "text": "UNDERTOW", "color": [1, 0.95, 0.8, 1], "height": 1.1, "layer": 95},
           Parallax={"factor": [0, 0]}, HudText={"kind": "Banner"})
    create("HUD Hint", (0, -half_h + 1.0),
           TextLabel={"font": font, "text": "", "color": [0.7, 0.85, 0.95, 1], "height": 0.42, "layer": 95},
           Parallax={"factor": [0, 0]}, HudText={"kind": "Hint"})

    # ---- rock: colliders per rectangle, lit tiles on the boundary, dark fill inside ------------
    for (x0, y0, x1, y1) in rectangles(grid):
        hw, hh = (x1 - x0 + 1) / 2, (y1 - y0 + 1) / 2
        create(f"Rock {x0},{y0}", (x0 + hw, y0 + hh), RigidBody2D=world_body,
               Collider2D={"shape": {"half": [hw, hh]}, "density": 0, "friction": 0.4, "restitution": 0.2})
    boundary = boundary_tiles(grid)
    stamp("Rock Face", [(x + 0.5, y + 0.5) for x, y in boundary],
          Sprite={"texture": tex("rock"), "normal_map": tex("rock_n"), "size": [1, 1], "layer": -5})
    for (x0, y0, x1, y1) in interior_rectangles(grid, boundary):
        create(f"Rock Fill {x0},{y0}", (x0 + (x1 - x0 + 1) / 2, y0 + (y1 - y0 + 1) / 2),
               Sprite={"texture": tex("rock_fill"), "size": [x1 - x0 + 1, y1 - y0 + 1], "layer": -6})

    # ---- lights and ambience ------------------------------------------------------------------
    stamp("Crystal", [(x + 0.5, y + 0.5) for x, y in CRYSTALS],
          Sprite={"texture": tex("crystal"), "size": [0.75, 1.0], "layer": -2, "tint": [0.8, 0.95, 1, 1]},
          Light2D={"color": [0.45, 0.85, 1, 1], "intensity": 1.7, "radius": 7.5, "height": 1.2}, Flicker={"amount": 0.3, "speed": 1.3})
    # Each crystal flickers out of phase: a different seed per entity (component_set with a path).
    crystals = m.tool("scene_list_entities", component="Flicker")["entities"]
    for i, e in enumerate(crystals):
        m.tool("component_set", entity=e["id"], component="Flicker", path="seed", value=i * 7 + 1)
    stamp("Bubbles", [(x + 0.5, y + 0.5) for x, y in BUBBLES],
          ParticleEmitter={"preset": fx("bubbles"), "seed": 3, "layer": 2, "playing": True})

    # ---- the drone -----------------------------------------------------------------------------
    drone_body = {"type": "Dynamic", "fixed_rotation": True, "layer": 1, "mask": 13, "gravity_scale": 0.25, "linear_damping": 0.9}
    drone = create("Drone", DRONE_START, RigidBody2D=drone_body,
                   Collider2D={"shape": {"type": "Circle", "radius": 0.45}, "density": 1, "friction": 0.2, "restitution": 0.35},
                   Drone={"bump_sound": sfx("bump"), "hit_sound": sfx("hit"), "explode_sound": sfx("explode"),
                          "tether_on_sound": sfx("tether_on"), "tether_off_sound": sfx("tether_off")},
                   AudioSource={"clip": sfx("hum"), "gain_db": -34, "bus": 2, "loop": True, "play_on_start": True, "priority": 200})
    if attract:
        route = {f"w{i}": [x, y] for i, (x, y) in enumerate(ATTRACT_ROUTE)}
        m.tool("component_add", entity=drone, component="Autopilot",
               value=dict(route, count=len(ATTRACT_ROUTE), tether_mask=1 << ATTRACT_TETHER_AT, loop=0, cruise=5.0, reach=1.0))
    create("Drone Visual", DRONE_START,
           Sprite={"texture": tex("drone"), "size": [1.3, 1.3], "layer": 10},
           Light2D={"color": [1, 0.95, 0.85, 1], "intensity": 2.0, "radius": 11, "height": 3.0}, DroneVisual={})
    create("Tether", DRONE_START, Sprite={"texture": tex("white"), "size": [1, 0.1], "pivot": [0, 0.5], "layer": 8,
                                           "tint": [1, 1, 0.8, 0]}, TetherVisual={"thickness": 0.09})
    for name, direction, offset in (("Right", "Right", (-0.6, 0.0)), ("Left", "Left", (0.6, 0.0)),
                                    ("Up", "Up", (0.0, -0.55)), ("Down", "Down", (0.0, 0.5))):
        plume = "thrust_" + {"Right": "left", "Left": "right", "Up": "down", "Down": "up"}[direction]
        create(f"Thruster {name}", DRONE_START, ParticleEmitter={"preset": fx(plume), "seed": 5, "layer": 9, "playing": False},
               Thruster={"offset": list(offset), "dir": direction})
    create("Boom", (0, 0), ParticleEmitter={"preset": fx("boom"), "seed": 9, "layer": 12, "playing": False})
    create("Sparks", (0, 0), ParticleEmitter={"preset": fx("spark"), "seed": 11, "layer": 12, "playing": False})

    # ---- pads, beacons, cores ------------------------------------------------------------------
    create("Dock", DOCK, RigidBody2D=pad_sensor, Collider2D={"shape": {"half": [2.2, 1.3], "offset": [0, 0.6]}, "density": 0, "is_sensor": True},
           Sprite={"texture": tex("dock"), "size": [4.4, 1.1], "layer": 1},
           Light2D={"color": [0.6, 1, 0.85, 1], "intensity": 1.3, "radius": 6, "height": 1.5},
           Pad={"kind": "Dock", "sound": sfx("pad"), "deliver_sound": sfx("deliver")})
    beacons = stamp("Beacon", [(DOCK[0] + (i - 2) * 1.1, DOCK[1] + 0.62) for i in range(5)],
                    Sprite={"texture": tex("beacon"), "size": [0.28, 0.28], "layer": 3, "tint": [0.25, 0.3, 0.35, 1]},
                    Light2D={"color": [0.55, 1, 0.8, 1], "intensity": 0, "radius": 4.5, "height": 1.0}, Beacon={"index": 0})
    for i, b in enumerate(beacons):
        m.tool("component_set", entity=b, component="Beacon", path="index", value=i)
    create("Relay", RELAY, RigidBody2D=pad_sensor, Collider2D={"shape": {"half": [1.2, 0.9], "offset": [0, 0.5]}, "density": 0, "is_sensor": True},
           Sprite={"texture": tex("relay"), "size": [2.4, 0.8], "layer": 1},
           Light2D={"color": [0.6, 1, 0.85, 1], "intensity": 1.0, "radius": 5, "height": 1.5},
           Pad={"kind": "Relay", "sound": sfx("pad"), "deliver_sound": sfx("deliver")})
    pod_body = {"type": "Dynamic", "fixed_rotation": False, "layer": 2, "mask": 15, "gravity_scale": 0.3, "linear_damping": 1.1}
    pods = stamp("Core", CORES, RigidBody2D=pod_body,
                 Collider2D={"shape": {"type": "Circle", "radius": 0.4}, "density": 1.6, "friction": 0.3, "restitution": 0.2},
                 Sprite={"texture": tex("pod"), "size": [0.9, 0.9], "layer": 6},
                 Light2D={"color": [0.6, 0.9, 1, 1], "intensity": 0.8, "radius": 3.5, "height": 1.0}, Cargo={"index": 0})
    for i, p in enumerate(pods):
        m.tool("component_set", entity=p, component="Cargo", path="index", value=i)

    # ---- hazards -------------------------------------------------------------------------------
    for i, ((cx, cy), (ex, ey), period, phase, (sw, sh)) in enumerate(CRUSHERS):
        create(f"Crusher {i}", (cx, cy), RigidBody2D={"type": "Kinematic", "fixed_rotation": True, "layer": 0, "gravity_scale": 0},
               Collider2D={"shape": {"half": [sw / 2, sh / 2]}, "density": 0},
               Sprite={"texture": tex("crusher"), "normal_map": tex("crusher_n"), "size": [sw, sh], "layer": 4},
               Mover={"extent": [ex, ey], "period": period, "phase": phase}, Hazard={"damage": 1})
    stamp("Mine", MINES, RigidBody2D=sensor, Collider2D={"shape": {"type": "Circle", "radius": 0.5}, "density": 0, "is_sensor": True},
          Sprite={"texture": tex("mine"), "size": [1.0, 1.0], "layer": 5},
          Light2D={"color": [1, 0.3, 0.25, 1], "intensity": 0.6, "radius": 2.5, "height": 0.8}, Mine={"sound": sfx("explode")},
          Flicker={"amount": 0.6, "speed": 2.0, "seed": 3})
    for (gx, gy), (sw, sh), channel in GATES:
        create(f"Gate {channel}", (gx, gy), RigidBody2D=world_body, Collider2D={"shape": {"half": [sw / 2, sh / 2]}, "density": 0},
               Sprite={"texture": tex("gate_h"), "size": [sw, sh], "layer": 5},
               Light2D={"color": [1, 0.35, 0.3, 1], "intensity": 1.2, "radius": 3.5, "height": 1.0}, Gate={"channel": channel})
    for i, ((sx, sy), channel, secs) in enumerate(SWITCHES):
        create(f"Switch {i}", (sx, sy), RigidBody2D=sensor, Collider2D={"shape": {"half": [0.45, 0.45]}, "density": 0, "is_sensor": True},
               Sprite={"texture": tex("switch"), "size": [0.9, 0.9], "layer": 5},
               Switch={"channel": channel, "open_seconds": secs, "sound": sfx("switch")})
    for i, ((cx, cy), (hx, hy), (fx_, fy)) in enumerate(CURRENTS):
        create(f"Current {i}", (cx, cy), Current={"force": [fx_, fy], "half": [hx, hy]})
        preset = "current_" + ("right" if fx_ > 0 else "left" if fx_ < 0 else "up" if fy > 0 else "down")
        spots = []
        xs = [cx] if hx <= 1.5 else [cx - hx + 1.5 + k * 3.0 for k in range(int((2 * hx - 3) / 3.0) + 1)]
        ys = [cy] if hy <= 1.5 else [cy - hy + 1.5 + k * 3.0 for k in range(int((2 * hy - 3) / 3.0) + 1)]
        for x in xs:
            for y in ys:
                spots.append((x, y))
        stamp(f"Flow {i}", spots, ParticleEmitter={"preset": fx(preset), "seed": 20 + i, "layer": 1, "playing": True})

    # ---- a few checks through the read tools --------------------------------------------------
    info = m.tool("scene_info")
    got = m.tool("scene_get_entity", entity=drone)
    drone_comp = got.get("components", {}).get("Drone")
    if drone_comp is None:
        note(friction, "bug", f"scene_get_entity did not return the Drone component: {json.dumps(got)[:200]}")
    elif "thrust" not in drone_comp:
        note(friction, "gap", "scene_get_entity returns only the fields the document authored (Drone.thrust, left at its default, is absent) and refl_describe_component has no defaults, so a client cannot read effective values without run_headless inspect")
    # Undo / redo on a deliberate mistake: move the drone into rock, then undo.
    m.tool("component_set", entity=drone, component="LocalTransform", path="position", value=[1, 1, 0])
    m.tool("scene_undo")
    pos = m.tool("scene_get_entity", entity=drone)["components"]["LocalTransform"]["position"]
    if abs(pos[0] - DRONE_START[0]) > 1e-5:
        note(friction, "bug", f"scene_undo did not restore the drone position: {pos}")
    m.tool("scene_redo")
    m.tool("scene_undo")
    # A type error must be refused and change nothing.
    err = m.tool("component_set", _expect_error=True, entity=drone, component="Drone", path="thrust", value="fast")
    if "error" not in err:
        note(friction, "bug", "component_set accepted a string for a float field")
    err = m.tool("component_set", _expect_error=True, entity=drone, component="Drone", path="hull", value=9)
    if "error" not in err:
        note(friction, "bug", "component_set accepted a write to a Transient field")
    saved = m.tool("scene_save", path=scene_path, force=True)
    return saved, info, grid, drone


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcp", required=True, help="the project's MCP server (undertow_mcp)")
    ap.add_argument("--transcript", help="write every JSON-RPC line here")
    ap.add_argument("--friction", help="write the friction notes (JSON) here")
    ap.add_argument("--ticks", type=int, default=2400, help="run_headless ticks for the attract scene")
    args = ap.parse_args()

    friction = []
    t_start = time.perf_counter()
    m = Mcp(args.mcp, ROOT, args.transcript, friction)

    # Discover: the game's components are known to this server.
    names = {c["name"] for c in m.tool("refl_list_components")["components"]}
    for need in ("Drone", "DroneVisual", "Thruster", "Cargo", "Pad", "Beacon", "Switch", "Gate", "Hazard", "Mine", "Current",
                 "Mover", "Flicker", "TetherVisual", "ScreenFlash", "CameraRig", "HudText", "RunStats", "Autopilot"):
        if need not in names:
            sys.exit(f"author_level_mcp: undertow_mcp does not know component {need}")
    desc = m.tool("refl_describe_component", name="AudioSource")
    bus = next((f for f in desc["fields"] if f["name"] == "bus"), None)
    if bus and bus.get("type") not in ("u8", "uint8", "int"):
        note(friction, "doc", f"AudioSource.bus is reflected as {bus.get('type')!r}: the bus ids (1 = music, 2 = sfx) are not an enum, a client has to know the numbers")

    # Register the generated art and sounds (sidecars), then fix the import settings the adopt
    # path cannot know: normal maps and the LUT are data, not colour (srgb off).
    cook = m.tool("cook_run", adopt=True)
    patched = 0
    for name in ("rock_n", "crusher_n", "lut_teal"):
        side = ROOT / "assets" / "textures" / f"{name}.png.mceasset"
        doc = json.loads(side.read_text(encoding="utf-8"))
        if doc.get("settings", {}).get("srgb", True):
            if m.has_tool("asset_set_import_settings"):
                m.tool("asset_set_import_settings", path=f"textures/{name}.png", settings={"srgb": False, "mips": False})
            else:
                doc["settings"] = {"srgb": False, "mips": False}
                side.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
            patched += 1
    if patched:
        note(friction, "gap",
             f"adopt wrote empty import settings (srgb defaults to on) for {patched} data textures (normal maps, LUT); "
             + ("set through asset_set_import_settings" if m.has_tool("asset_set_import_settings") else
                "the sidecars were patched on disk: no MCP tool sets import settings")
             + ", and nothing warned that a Sprite.normal_map texture is sRGB")
    assets = {a["path"]: a["guid"] for a in m.tool("asset_list")["assets"]}
    effects = ROOT / "assets" / "effects"
    effects.mkdir(exist_ok=True)
    existing = {p.stem: json.loads(p.read_text(encoding="utf-8"))["guid"] for p in effects.glob("*.mcpart")}

    import uuid
    presets = write_presets(effects, lambda n: assets[f"textures/{n}.png"], lambda n: existing.get(n) or str(uuid.uuid4()))
    note(friction, "gap", f"{len(presets)} particle presets (.mcpart) written to disk by the script: no MCP tool authors presets")
    cook = m.tool("cook_run")
    if not cook["ok"]:
        # A stale scene (one that names an asset the art generator no longer makes) fails the cook
        # before authoring; the registry still has every current file, which is all this needs.
        errs = m.tool("cook_errors")
        scene_only = all(d.get("file", "").endswith(".mcscene") for d in errs.get("diagnostics", []))
        if not scene_only:
            print(json.dumps(errs, indent=1)[:2000])
            sys.exit("author_level_mcp: the asset cook failed")
        note(friction, "gap", f"the previous scenes reference an asset that no longer exists ({cook['errors']} cook errors); "
             "authoring continues since the scenes are rebuilt, but a cook error in one scene blocks the whole project's pack")

    print("== main scene")
    saved, info, grid, drone = author(m, friction, "scenes/main.mcscene", attract=False)
    print("== attract scene")
    saved2, _, _, drone2 = author(m, friction, "scenes/attract.mcscene", attract=True)
    cook = m.tool("cook_run", pack=True)
    if not cook["ok"]:
        print(json.dumps(m.tool("cook_errors"), indent=1)[:3000])
        sys.exit("author_level_mcp: cook failed after authoring the scenes")
    run = m.tool("run_headless", ticks=args.ticks, log_lines=20,
                 inspect={"component": "RunStats", "entities": [drone2], "show": ["RunStats", "Drone", "LocalTransform", "Autopilot"]})
    print(f"  {saved2['entities']} entities saved; run_headless {args.ticks}: {json.dumps(run)[:1200]}")
    stats = None
    for e in run.get("inspect", {}).get("entities", []) if isinstance(run.get("inspect"), dict) else run.get("inspect", []):
        comps = e.get("components", {})
        if "RunStats" in comps:
            stats = comps["RunStats"]
    if stats is None:
        note(friction, "bug?", f"run_headless inspect returned no RunStats: {json.dumps(run.get('inspect'))[:300]}")
    elif int(stats.get("cores", 0)) < 1:
        note(friction, "design", f"the autopilot did not dock core 1 in {args.ticks} ticks: {stats}")
    else:
        print(f"  attract autopilot docked core 1: {stats}")

    # The main scene, idle (no input): the drone waits in the intro, nothing moves it.
    m.tool("scene_open", path="scenes/main.mcscene", force=True)
    run_main = m.tool("run_headless", ticks=300, log_lines=10, inspect={"component": "RunStats"})
    print(f"  main run_headless 300: {json.dumps(run_main)[:300]}")

    # Console and resources: what a client can read besides the scene.
    convars = m.tool("console_list", prefix="audio")
    res = m.resource("mce://scene/current")
    text = res["contents"][0]["text"]
    if '"Drone"' not in text:
        note(friction, "bug?", "mce://scene/current does not show the open scene's Drone component")
    m.close()

    elapsed = time.perf_counter() - t_start
    report = {
        "calls": m.calls, "seconds": round(elapsed, 2), "errors": m.errors,
        "timings": {k: {"calls": v[0], "total_s": round(v[1], 3), "max_s": round(v[2], 4)} for k, v in sorted(m.timings.items())},
        "notes": friction, "run_main": run_main, "run_attract": run, "convars_audio": convars,
    }
    if args.friction:
        pathlib.Path(args.friction).write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(ascii_map(grid))
    print(f"author_level_mcp: {m.calls} tool calls in {elapsed:.1f} s; main {saved['entities']} entities, attract {saved2['entities']}; "
          f"{len(friction)} friction notes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
