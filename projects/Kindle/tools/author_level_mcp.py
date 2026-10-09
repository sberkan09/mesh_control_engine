#!/usr/bin/env python3
"""Authors Kindle's level through the project's editor MCP server (kindle_mcp, editor-mcp.md).

This is the record of how assets/scenes/main.mcscene was made: an MCP client (this script, run by
an agent) speaks JSON-RPC to `kindle_mcp --project projects/Kindle` over stdio and issues the same
editor commands a person issues in kindle_editor: create an entity with its components, duplicate
it (Ctrl+D), move the copy (the gizmo, here `component_set LocalTransform.position`), save, cook,
run headless. Every write goes through the editor's EditorScene funnel. Nothing here writes the
.mcscene file itself; the server's scene_save does.

    python3 projects/Kindle/tools/author_level_mcp.py --mcp build/linux-gcc-debug/projects/Kindle/kindle_mcp
    python3 projects/Kindle/tools/author_level_mcp.py --mcp ... --transcript /tmp/kindle_mcp.jsonl

The layout below is the designer's sketch (one character = one 1 m tile, y = 0 is the bottom row).
The script turns it into editor actions; the GUIDs come from the server (asset_list), not from disk.
"""

import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# Legend: # ground   R red block   B blue block   S colour switch   o coin   ^ spikes
#         X crumbling block   F goal flag   P player start   V vertical moving platform (centre)
# The red wall (x 18) and the plateau (x 45) cannot be jumped; the 10-tile pit (x 21-30) cannot be
# cleared even with jump + dash. Path: the switch at x 14 swaps to blue (the wall goes, the bridge
# comes), the switch at x 35 swaps back to red (the stairs come), then over the crumbling bridge
# to the flag. The lift (V, 1.5 m up and down) carries the player to two bonus coins.
LEVEL = [
    "#                                                               #",  # y 13
    "#                                                               #",  # y 12
    "#                                                               #",  # y 11
    "#                                                               #",  # y 10
    "#                                oo                             #",  # y 9
    "#                                                               #",  # y 8
    "#                                                               #",  # y 7
    "#                 R                                o    o F     #",  # y 6
    "#                 R              V         RR#####XXX############",  # y 5
    "#                 R                        RR#####   ############",  # y 4
    "#                 R                    RR  RR#####   ############",  # y 3
    "#  P ooo  ^^  S   R      oo        S   RR  RR#####^^^############",  # y 2: the surface
    "#####################BBBBBBBBBB##################################",  # y 1
    "#####################          ##################################",  # y 0
]


class Mcp:
    def __init__(self, exe, project, transcript):
        self.proc = subprocess.Popen([exe, "--project", str(project)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.next_id = 1
        self.calls = 0
        self.log = open(transcript, "w", encoding="utf-8") if transcript else None
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "kindle-author", "version": "1"}})
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
                raise RuntimeError(f"kindle_mcp closed the pipe during {method}")
            if self.log:
                self.log.write(line if line.endswith("\n") else line + "\n")
            reply = json.loads(line)
            if reply.get("id") == rid:
                break  # notifications (progress) are skipped
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
        self.proc.wait(timeout=30)
        if self.log:
            self.log.close()


def tile(x, y):
    """The level sketch at (x, y), y = 0 the bottom row; outside is air."""
    row = len(LEVEL) - 1 - y
    if row < 0 or row >= len(LEVEL) or x < 0 or x >= len(LEVEL[row]):
        return " "
    return LEVEL[row][x]


def width():
    return max(len(r) for r in LEVEL)


def rectangles(ch):
    """Greedy cover of the `ch` tiles by rectangles: one static collider each."""
    used = set()
    out = []
    for y in range(len(LEVEL)):
        for x in range(width()):
            if tile(x, y) != ch or (x, y) in used:
                continue
            x1 = x
            while tile(x1 + 1, y) == ch and (x1 + 1, y) not in used:
                x1 += 1
            y1 = y
            while all(tile(i, y1 + 1) == ch and (i, y1 + 1) not in used for i in range(x, x1 + 1)):
                y1 += 1
            for j in range(y, y1 + 1):
                for i in range(x, x1 + 1):
                    used.add((i, j))
            out.append((x, y, x1, y1))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcp", required=True, help="the project's MCP server (kindle_mcp)")
    ap.add_argument("--transcript", help="write every JSON-RPC line here")
    ap.add_argument("--scene", default="scenes/main.mcscene")
    args = ap.parse_args()

    m = Mcp(args.mcp, ROOT, args.transcript)

    # Discover: the game's components are known to this server; the assets come from its registry.
    names = {c["name"] for c in m.tool("refl_list_components")["components"]}
    for need in ("PlayerController", "SwitchBlock", "ColorSwitch", "Coin", "Hazard", "Goal", "Crumble", "Mover",
                 "CameraFollow", "HudText"):
        if need not in names:
            sys.exit(f"author_level_mcp: kindle_mcp does not know component {need}")
    assets = {a["path"]: a["guid"] for a in m.tool("asset_list")["assets"]}

    def guid(path):
        return assets[path]

    tex = lambda name: guid(f"textures/{name}.png")  # noqa: E731
    sfx = lambda name: guid(f"sfx/{name}.wav")  # noqa: E731

    # Re-authoring keeps the scene's GUID (Kindle.mceproj names it): open it and clear it.
    if (ROOT / "assets" / args.scene).exists():
        m.tool("scene_open", path=args.scene, force=True)
        for e in m.tool("scene_list_entities")["entities"]:
            m.tool("entity_delete", entity=e["id"])
    else:
        m.tool("scene_new", name="main", force=True)

    def create(name, pos, **components):
        comps = {"LocalTransform": {"position": [pos[0], pos[1], 0]}}
        comps.update(components)
        return m.tool("entity_create", name=name, components=comps)["id"]

    def place(proto, pos):
        """Duplicate an entity and move the copy (Ctrl+D, then the move gizmo)."""
        copy = m.tool("entity_duplicate", entity=proto)["ids"][0]
        m.tool("component_set", entity=copy, component="LocalTransform", path="position", value=[pos[0], pos[1], 0])
        return copy

    def stamp(name, positions, **components):
        """One authored entity, then duplicates for the rest."""
        if not positions:
            return []
        first = create(name, positions[0], **components)
        return [first] + [place(first, p) for p in positions[1:]]

    world_body = {"type": "Static", "layer": 0}
    sensor_body = {"type": "Static", "layer": 3, "mask": 2}  # sensors see the player layer only

    # ---- camera, backdrop and HUD ---------------------------------------------------------------
    w = width()
    half_w = 1280 / 48 / 2
    cam_y = 720 / 48 / 2  # the view's bottom edge on y = 0
    create("Camera", (half_w, cam_y),
           Camera2DComponent={"pixels_per_meter": 48, "clear_color": [0.42, 0.62, 0.86, 1]},
           CameraFollow={"offset": [0, 0], "min": [half_w, cam_y], "max": [w - half_w, cam_y], "smoothing": 0.18})
    create("Sky", (0, 0), Sprite={"texture": tex("backdrop/sky"), "size": [30, 16], "layer": -20},
           Parallax={"factor": [0, 0], "repeat_x": True})
    create("Mountains", (0, -1.5), Sprite={"texture": tex("backdrop/mountains"), "size": [40, 12], "layer": -16},
           Parallax={"factor": [0.15, 0], "repeat_x": True})
    create("Hills", (0, -4.5), Sprite={"texture": tex("backdrop/hills"), "size": [40, 8], "layer": -14},
           Parallax={"factor": [0.35, 0], "repeat_x": True})
    stamp("Cloud", [(6, 3.4), (19, 4.2), (31, 2.9), (44, 3.8)],
          Sprite={"texture": tex("ember/cloud"), "size": [2.5, 2.5], "layer": -12, "tint": [1, 1, 1, 0.9]},
          Parallax={"factor": [0.6, 0]})
    font = guid("fonts/ProggyClean.ttf")
    create("HUD Status", (-half_w + 0.6, 6.6),
           TextLabel={"font": font, "text": "COINS 0/0", "color": [1, 0.97, 0.88, 1], "height": 0.55, "layer": 60, "align": "Left"},
           Parallax={"factor": [0, 0]}, HudText={"kind": "Status"})
    create("HUD Banner", (0, 5.7),
           TextLabel={"font": font, "text": "SWITCHES SWAP RED AND BLUE - LIGHT THE FLAG", "color": [1, 0.85, 0.5, 1],
                      "height": 0.5, "layer": 60},
           Parallax={"factor": [0, 0]}, HudText={"kind": "Banner"})

    # ---- ground: one collider per rectangle, one sprite per tile ------------------------------
    for (x0, y0, x1, y1) in rectangles("#"):
        hw, hh = (x1 - x0 + 1) / 2, (y1 - y0 + 1) / 2
        create(f"Ground {x0}-{x1}", (x0 + hw, y0 + hh), RigidBody2D=world_body,
               Collider2D={"shape": {"half": [hw, hh]}, "density": 0})
    top = [(x + 0.5, y + 0.5) for y in range(len(LEVEL)) for x in range(w) if tile(x, y) == "#" and tile(x, y + 1) != "#"]
    fill = [(x + 0.5, y + 0.5) for y in range(len(LEVEL)) for x in range(w) if tile(x, y) == "#" and tile(x, y + 1) == "#"]
    stamp("Grass", top, Sprite={"texture": tex("ember/ground_top"), "size": [1, 1]})
    stamp("Soil", fill, Sprite={"texture": tex("ember/ground_fill"), "size": [1, 1]})

    def cells(ch):
        return [(x, y) for y in range(len(LEVEL)) for x in range(w) if tile(x, y) == ch]

    # ---- the new mechanic: switch blocks and switches ------------------------------------------
    block_collider = {"shape": {"half": [0.5, 0.5]}, "density": 0}
    stamp("Red Block", [(x + 0.5, y + 0.5) for x, y in cells("R")], RigidBody2D=world_body, Collider2D=block_collider,
          Sprite={"texture": tex("switch/red_block"), "size": [1, 1], "layer": 1}, SwitchBlock={"color": "Red"})
    stamp("Blue Block", [(x + 0.5, y + 0.5) for x, y in cells("B")], RigidBody2D=world_body, Collider2D=block_collider,
          Sprite={"texture": tex("switch/blue_block"), "size": [1, 1], "layer": 1}, SwitchBlock={"color": "Blue"})
    stamp("Switch", [(x + 0.5, y + 0.5) for x, y in cells("S")], RigidBody2D=sensor_body,
          Collider2D={"shape": {"half": [0.4, 0.5]}, "density": 0, "is_sensor": True},
          Sprite={"texture": tex("switch/switch"), "size": [1, 1], "layer": 2},
          ColorSwitch={"sound": sfx("stomp"), "start_color": "Red"})

    # ---- Ember Step's pieces -------------------------------------------------------------------
    stamp("Coin", [(x + 0.5, y + 0.5) for x, y in cells("o")], RigidBody2D=sensor_body,
          Collider2D={"shape": {"type": "Circle", "radius": 0.3}, "density": 0, "is_sensor": True},
          Sprite={"texture": tex("ember/coin"), "size": [0.75, 0.75], "layer": 2}, Coin={"sound": sfx("pickup")})
    stamp("Spikes", [(x + 0.5, y + 0.3) for x, y in cells("^")], RigidBody2D=sensor_body,
          Collider2D={"shape": {"half": [0.4, 0.2]}, "density": 0, "is_sensor": True},
          Sprite={"texture": tex("ember/spike"), "size": [1, 0.6], "layer": 1}, Hazard={})
    stamp("Crumbling Block", [(x + 0.5, y + 0.5) for x, y in cells("X")], RigidBody2D=world_body,
          Collider2D=block_collider, Sprite={"texture": tex("ember/crumble"), "size": [1, 1], "layer": 1}, Crumble={})
    for x, y in cells("V"):
        create("Lift", (x + 0.5, y + 0.5),
               RigidBody2D={"type": "Kinematic", "fixed_rotation": True, "layer": 0, "gravity_scale": 0},
               Collider2D={"shape": {"half": [1.5, 0.25]}, "density": 0},
               Sprite={"texture": tex("ember/platform"), "size": [3, 0.5], "layer": 1},
               Mover={"extent": [0, 1.5], "period": 5, "phase": 0})
    for x, y in cells("F"):
        create("Flag", (x + 0.5, y + 1.0), RigidBody2D=sensor_body,
               Collider2D={"shape": {"half": [0.3, 1.0]}, "density": 0, "is_sensor": True},
               Sprite={"texture": tex("ember/flag"), "size": [1, 2], "layer": 1}, Goal={"sound": sfx("win")})
    for x, y in cells("P"):
        create("Player", (x + 0.5, y + 0.46),
               RigidBody2D={"type": "Kinematic", "fixed_rotation": True, "layer": 1, "mask": 9, "gravity_scale": 0},
               Collider2D={"shape": {"half": [0.35, 0.45]}, "density": 0},
               CharacterController2D={},
               Sprite={"texture": tex("ember/player_idle"), "size": [1, 1], "layer": 10},
               PlayerController={"idle": tex("ember/player_idle"), "run_a": tex("ember/player_run_a"),
                                 "run_b": tex("ember/player_run_b"), "jump": tex("ember/player_jump"),
                                 "dash": tex("ember/player_dash"), "jump_sound": sfx("jump"), "hurt_sound": sfx("hurt")})

    saved = m.tool("scene_save", path=args.scene, force=True)
    cook = m.tool("cook_run")
    run = m.tool("run_headless", ticks=300, log_lines=0)
    m.close()
    print(f"author_level_mcp: {m.calls} tool calls; {saved['entities']} entities saved to {saved['path']} "
          f"(guid {saved['guid']}); cook ok={cook['ok']} cooked={cook['cooked']} errors={cook['errors']}; "
          f"run_headless {json.dumps(run)[:300]}")
    return 0 if cook["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
