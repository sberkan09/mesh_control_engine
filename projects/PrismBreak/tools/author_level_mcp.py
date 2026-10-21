#!/usr/bin/env python3
"""Authors Prism Break's level through the project's editor MCP server (prism_break_mcp).

This is the record of how assets/scenes/main.mcscene was made: an MCP client (this script, run by
an agent) speaks JSON-RPC to `prism_break_mcp --project projects/PrismBreak` over stdio and issues
editor commands: open the template scene, clear it, create the arena, the paddle, the ball and one
brick of each kind, duplicate bricks into the wall, save, cook, run headless. Every write goes
through the editor's EditorScene funnel; nothing here writes the .mcscene file itself.

    python3 projects/PrismBreak/tools/author_level_mcp.py --mcp build/linux-gcc-debug/projects/PrismBreak/prism_break_mcp
    python3 projects/PrismBreak/tools/author_level_mcp.py --mcp ... --transcript /tmp/prism_mcp.jsonl

The wall below is the designer's sketch: one character per brick, top row first.
"""

import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# Legend: 1-3 a normal brick with that many hit points   P prism brick (splits the ball)
#         S steel brick (never breaks)   space: no brick
WALL = [
    "S333333333S",
    "22222P22222",
    "2P2222222P2",
    "11111111111",
    "111S111S111",
]
BRICK_W, BRICK_H = 1.6, 0.6     # metres
PITCH_X, PITCH_Y = 1.7, 0.7
TOP_ROW_Y = 4.6

ARENA_HALF_W = 10.0             # inner walls at x = +-10
TOP_Y = 5.6                     # inner face of the top wall
FLOOR_Y = -7.0
PADDLE_Y = -6.2
PADDLE_HALF = (1.2, 0.22)
BALL_RADIUS = 0.22


class Mcp:
    def __init__(self, exe, project, transcript):
        self.proc = subprocess.Popen([exe, "--project", str(project)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.next_id = 1
        self.calls = 0
        self.log = open(transcript, "w", encoding="utf-8") if transcript else None
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "prism-author", "version": "1"}})
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
                raise RuntimeError(f"prism_break_mcp closed the pipe during {method}")
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


def guids(mcp):
    """Asset path -> "guid:<uuid>", from the server's GUID registry."""
    out = {}
    for a in mcp.tool("asset_list")["assets"]:
        out[a["path"]] = a["guid"]
    return out


def static_box(name, x, y, hx, hy, texture=None, layer=0):
    comps = {
        "LocalTransform": {"position": [x, y, 0]},
        "RigidBody2D": {},
        "Collider2D": {"shape": {"half": [hx, hy]}, "density": 0, "friction": 0},
    }
    if texture:
        comps["Sprite"] = {"texture": texture, "size": [2 * hx, 2 * hy], "layer": layer}
    return name, comps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcp", required=True, help="path to prism_break_mcp")
    ap.add_argument("--project", default=str(ROOT))
    ap.add_argument("--transcript", help="write the JSON-RPC traffic here (JSONL)")
    ap.add_argument("--ticks", type=int, default=12000, help="run_headless ticks for the attract-mode check at the end")
    args = ap.parse_args()

    mcp = Mcp(args.mcp, args.project, args.transcript)
    try:
        mcp.tool("scene_open", path="scenes/main.mcscene")
    except RuntimeError as e:
        # The template's scene uses the template's Spin / Bob components, which this game removed:
        # the editor cannot open a scene with unknown components, so start a new one (fresh GUID).
        print(f"note: {e}\nnote: starting a new scene instead", file=sys.stderr)
        mcp.tool("scene_new", name="main", force=True)

    # Start from an empty scene: the template's demo entities go.
    for e in mcp.tool("scene_list_entities")["entities"]:
        mcp.tool("entity_delete", entity=e["id"])

    # Art, sounds and the font are plain files dropped into assets/ (tools/gen_art.py, copies):
    # adopt gives the new ones a .mceasset sidecar (a fresh GUID) so the cook and asset_list see them.
    cook = mcp.tool("cook_run", adopt=True)
    adopted = [d["file"] for d in cook["diagnostics"] if "adopted" in d["message"]]
    if adopted:
        print("adopted: " + ", ".join(adopted), file=sys.stderr)
    g = guids(mcp)
    tex = lambda name: g[f"textures/{name}.png"]
    sfx = lambda name: g[f"sfx/{name}.wav"]
    font = g["fonts/ProggyClean.ttf"]

    def create(name, comps):
        return mcp.tool("entity_create", name=name, components=comps)["id"]

    create("Camera", {"LocalTransform": {"position": [0, 0, 0]},
                      "Camera2DComponent": {"pixels_per_meter": 48, "clear_color": [0.05, 0.05, 0.1, 1]}})
    create("Backdrop", {"LocalTransform": {"position": [0, 0, 0]},
                        "Sprite": {"texture": tex("backdrop"), "size": [28, 16], "layer": -20}})
    create("Rules", {"Rules": {"brick_sound": sfx("pickup"), "paddle_sound": sfx("jump"), "lose_sound": sfx("hurt"),
                               "win_sound": sfx("win"), "lives": 3, "kill_y": FLOOR_Y - 0.8}})

    # The arena: walls left, right and top (the bottom is open).
    side_h = (TOP_Y + 0.8 - FLOOR_Y) / 2
    for name, x, y, hx, hy in [("Wall Left", -ARENA_HALF_W - 0.4, FLOOR_Y + side_h, 0.4, side_h),
                               ("Wall Right", ARENA_HALF_W + 0.4, FLOOR_Y + side_h, 0.4, side_h),
                               ("Wall Top", 0, TOP_Y + 0.4, ARENA_HALF_W + 0.8, 0.4)]:
        create(*static_box(name, x, y, hx, hy, tex("wall"), layer=-5))

    reach = ARENA_HALF_W - PADDLE_HALF[0]
    paddle = create("Paddle", {"LocalTransform": {"position": [0, PADDLE_Y, 0]},
                      "RigidBody2D": {"type": "Kinematic", "fixed_rotation": True, "gravity_scale": 0},
                      "Collider2D": {"shape": {"half": list(PADDLE_HALF)}, "density": 0, "friction": 0},
                      "Paddle": {"min_x": -reach, "max_x": reach},
                      "Sprite": {"texture": tex("paddle"), "size": [2 * PADDLE_HALF[0], 2 * PADDLE_HALF[1] + 0.05],
                                 "layer": 5}})
    create("Ball", {"LocalTransform": {"position": [0, PADDLE_Y + PADDLE_HALF[1] + BALL_RADIUS + 0.02, 0]},
                    "Ball": {"radius": BALL_RADIUS},
                    "Sprite": {"texture": tex("ball"), "size": [2 * BALL_RADIUS, 2 * BALL_RADIUS], "layer": 10}})

    hud = {"font": font, "color": [0.92, 0.95, 1, 1], "height": 0.5, "layer": 60}
    create("HUD Status", {"LocalTransform": {"position": [-ARENA_HALF_W - 0.8, 6.95, 0]}, "HudText": {"kind": "Status"},
                          "TextLabel": dict(hud, text="SCORE 0", align="Left")})
    create("HUD Banner", {"LocalTransform": {"position": [0, -2.5, 0]}, "HudText": {"kind": "Banner"},
                          "TextLabel": dict(hud, text="SPACE: serve", height=0.6, align="Center")})

    # The wall: one brick of each kind made by hand, the rest duplicated from it (Ctrl+D) and moved.
    hx, hy = BRICK_W / 2, BRICK_H / 2
    proto = {}
    for ch in sorted({c for row in WALL for c in row if c != " "}):
        kind = {"P": "Prism", "S": "Steel"}.get(ch, "Normal")
        hp = int(ch) if ch.isdigit() else 1
        name, comps = static_box(f"Brick {kind}" + (f" {hp}" if kind == "Normal" else ""), 0, 0, hx, hy, tex("brick"), 0)
        comps["Brick"] = {"kind": kind, "max_hp": hp, "points": 10 * hp if kind == "Normal" else 50}
        proto[ch] = create(name, comps)
    placed = set()
    cols = len(WALL[0])
    for r, row in enumerate(WALL):
        for c, ch in enumerate(row):
            if ch == " ":
                continue
            x = round((c - (cols - 1) / 2) * PITCH_X, 4)
            y = round(TOP_ROW_Y - r * PITCH_Y, 4)
            if ch in placed:
                eid = mcp.tool("entity_duplicate", entity=proto[ch])["ids"][0]
            else:
                eid = proto[ch]
                placed.add(ch)
            mcp.tool("component_set", entity=eid, component="LocalTransform", path="position", value=[x, y, 0])

    mcp.tool("scene_save", path="scenes/main.mcscene", force=True)
    info = mcp.tool("scene_info")

    # The attract-mode copy: the same level with the paddle on autopilot (save as: a fresh GUID).
    # `prism_break -scene scenes/attract.mcscene` plays itself; the headless checks use it too.
    mcp.tool("component_set", entity=paddle, component="Paddle", path="autopilot", value=1)
    mcp.tool("scene_save", path="scenes/attract.mcscene", force=True)
    cook = mcp.tool("cook_run")

    # The check: play the attract scene (the game's own systems run in a project server) and read
    # the bricks back. The autopilot clears the wall in about 11 000 ticks.
    run = mcp.tool("run_headless", ticks=args.ticks, log_lines=50, inspect={"component": "Brick", "show": ["Brick"]})
    bricks = run["inspect"]["entities"]
    standing = [b for b in bricks if b["components"]["Brick"]["alive"] and b["components"]["Brick"]["kind"] != "Steel"]
    cleared = any("cleared" in line for line in json.dumps(run["log"]["tail"]).split("prism_break:"))
    mcp.close()
    start = json.loads((pathlib.Path(args.project) / f"{pathlib.Path(args.project).name}.mceproj").read_text())["start_scene"]
    if start != info["guid"]:
        print(f"warning: the .mceproj start_scene is {start}, the scene's GUID is {info['guid']}: "
              "update start_scene (no MCP tool edits the project file)", file=sys.stderr)
    print(json.dumps({"scene": info, "cook_ok": cook["ok"], "run": {k: run[k] for k in ("ticks", "entities", "state_hash")},
                      "bricks": len(bricks), "breakable_standing": len(standing), "cleared": cleared,
                      "tool_calls": mcp.calls}, indent=2))
    return 0 if cook["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
