#!/usr/bin/env python3
"""Authors Undelivered's one scene through the project's editor MCP server (undelivered_mcp).

The record of how assets/scenes/main.mcscene was made: this script speaks JSON-RPC to
`undelivered_mcp --project projects/Undelivered` over stdio and issues the editor's own commands
(create an entity with its components, set import settings, save, cook, run headless). Nothing
here writes the .mcscene file itself.

    python3 projects/Undelivered/tools/author_scene_mcp.py \
        --mcp build/linux-gcc-debug/projects/Undelivered/undelivered_mcp

A light novel's scene is a stage: every part of the screen (the paper layers of a location, the
three cast slots, the box, the choice rows, the letter card, the panel, the envelopes, the lights,
the two looping sound beds) is one entity tagged with a UiPart; the game writes into them at
runtime (games/undelivered/module/undelivered.cpp). The view is 20 x 11.25 m (1280 x 720 at 64
pixels per metre), the origin at the centre.
"""

import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

PPM = 64.0
VIEW_W, VIEW_H = 1280 / PPM, 720 / PPM
PROSE_FONT = "fonts/CrimsonPro-Regular.ttf"
BOLD_FONT = "fonts/CrimsonPro-Bold.ttf"
HAND_FONT = "fonts/NothingYouCouldDo-Regular.ttf"

INK = [0.17, 0.13, 0.1, 1]
PAPER_TEXT = [0.92, 0.88, 0.8, 1]


class Mcp:
    def __init__(self, exe, project, transcript):
        self.proc = subprocess.Popen([exe, "--project", str(project)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.next_id = 1
        self.calls = 0
        self.log = open(transcript, "w", encoding="utf-8") if transcript else None
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "undelivered-author", "version": "1"}})
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
                raise RuntimeError(f"undelivered_mcp closed the pipe during {method}")
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


def px(w, h):
    return [round(w / PPM, 4), round(h / PPM, 4)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcp", required=True, help="the project's MCP server (undelivered_mcp)")
    ap.add_argument("--transcript", help="write every JSON-RPC line here")
    ap.add_argument("--scene", default="scenes/main.mcscene")
    ap.add_argument("--ticks", type=int, default=300, help="run_headless ticks at the end")
    args = ap.parse_args()

    m = Mcp(args.mcp, ROOT, args.transcript)
    names = {c["name"] for c in m.tool("refl_list_components")["components"]}
    for need in ("UiPart", "NovelSave", "SaveKey", "Sprite", "TextLabel", "Light2D", "AudioSource", "Post2DComponent"):
        if need not in names:
            sys.exit(f"author_scene_mcp: undelivered_mcp does not know component {need}")

    # New files (the generator's textures and sounds) get sidecars from an adopting cook first.
    m.tool("cook_run", adopt=True)
    assets = {a["path"]: a["guid"] for a in m.tool("asset_list")["assets"]}

    def guid(path):
        g = assets[path]
        return g if g.startswith("guid:") else "guid:" + g

    # Data textures are not colour: the paper normal map and the five LUT strips import with srgb off.
    linear = ["textures/paper_normal.png"] + [p for p in assets if p.startswith("textures/lut_")]
    for p in linear:
        m.tool("asset_set_import_settings", path=p, settings={"srgb": False, "mips": False})

    # Re-authoring keeps the scene's GUID (Undelivered.mceproj names it): open it and clear it.
    if (ROOT / "assets" / args.scene).exists():
        m.tool("scene_open", path=args.scene, force=True)
        m.tool("scene_clear")
    else:
        m.tool("scene_new", name="main", force=True)

    created = 0

    def create(name, role, index=0, pos=(0, 0, 0), scale=None, rot=None, **components):
        nonlocal created
        tr = {"position": [round(float(c), 4) for c in pos]}
        if scale is not None:
            tr["scale"] = list(scale)
        if rot is not None:
            tr["rotation"] = rot
        comps = {"LocalTransform": tr, "UiPart": {"role": role, "index": index}}
        comps.update(components)
        created += 1
        return m.tool("entity_create", name=name, components=comps)["id"]

    def sprite(texture=None, size=(1, 1), layer=0, order=0, tint=(1, 1, 1, 1), pivot=(0.5, 0.5)):
        s = {"size": list(size), "layer": layer, "order": order, "tint": list(tint), "pivot": list(pivot)}
        if texture:
            s["texture"] = guid(texture)
        return s

    def label(font, height, align="Left", color=PAPER_TEXT, layer=8, text=""):
        return {"font": guid(font), "text": text, "height": height, "align": align, "color": list(color), "layer": layer}

    # ---- the view
    create("Camera", "Camera", pos=(0, 0, 0),
           Camera2DComponent={"pixels_per_meter": PPM, "clear_color": [0.05, 0.045, 0.04, 1], "pixel_snap": False},
           Post2DComponent={"vignette": True, "vignette_intensity": 0.38, "vignette_radius": 0.85, "vignette_softness": 0.7,
                            "color_grade": True, "lut": guid("textures/lut_neutral.png"), "lut_strength": 0.85})
    create("Story", "Save", NovelSave={}, SaveKey={"key": 1})

    # ---- the stage: five paper layers, lit by an ambient and a lamp that moves with the hour
    for k in range(5):
        create(f"Paper {k}", "Background", k, pos=(0, 0, 0),
               Sprite=sprite(None, size=(VIEW_W, VIEW_H), layer=0, order=k, tint=(1, 1, 1, 0)))
    create("Daylight", "AmbientLight", Light2D={"kind": "Ambient", "color": [1, 1, 1, 1], "intensity": 1.0})
    create("Lamp", "Light", pos=(0, 5, 0), Light2D={"kind": "Point", "color": [1, 0.95, 0.85, 1], "intensity": 1.0,
                                                   "radius": 12.0, "height": 2.5})

    # ---- the cast: three slots, cut-out dolls 360 x 720 px
    for i, x in enumerate((-5.5, 0.0, 5.5)):
        create(f"Cast {i}", "Cast", i, pos=(x, -1.0, 0), Sprite=sprite(None, size=px(360, 720), layer=2, tint=(1, 1, 1, 0)))

    # ---- shades
    create("Dim", "Dim", Sprite=sprite(None, size=(VIEW_W + 2, VIEW_H + 2), layer=5, tint=(0.02, 0.015, 0.01, 0)))
    create("Fade", "Fade", Sprite=sprite(None, size=(VIEW_W + 2, VIEW_H + 2), layer=12, tint=(0, 0, 0, 1)))
    create("Glow", "Glow", pos=(0, 3.0, 0), Sprite=sprite("textures/ui/glow.png", size=(16, 2.4), layer=6, tint=(1, 0.9, 0.7, 0)))

    # ---- the dialogue box: 1280 x 230 px along the bottom, the tag above its left corner
    box_y = -VIEW_H / 2 + 230 / PPM / 2
    create("Box", "Box", pos=(0, box_y, 0), Sprite=sprite("textures/ui/box.png", size=px(1280, 230), layer=6, tint=(1, 1, 1, 0)))
    create("Name tag", "NameTag", pos=(-6.3, -1.95, 0), Sprite=sprite("textures/ui/name_tag.png", size=px(320, 60), layer=7, tint=(1, 1, 1, 0)))
    create("Name", "NameText", pos=(-6.3, -1.95, 0), TextLabel=label(BOLD_FONT, 0.42, "Center", [1, 0.93, 0.8, 1], layer=8))
    for i in range(4):
        create(f"Line {i}", "Line", i, pos=(-6.7, -2.85 - i * 0.62, 0), TextLabel=label(PROSE_FONT, 0.5, "Left", layer=8))

    # ---- choice rows (also the title menu and the drawer's three actions)
    for i in range(4):
        y = 3.0 - 1.9 * i
        create(f"Row {i}", "ChoiceRow", i, pos=(0, y, 0), Sprite=sprite("textures/ui/choice_row.png", size=px(900, 62), layer=7, tint=(1, 1, 1, 0)))
        create(f"Row text {i}", "ChoiceText", i, pos=(-6.6, y, 0), TextLabel=label(PROSE_FONT, 0.5, "Left", layer=8))
        create(f"Row text {i} b", "ChoiceText2", i, pos=(-6.6, y - 0.37, 0), TextLabel=label(PROSE_FONT, 0.5, "Left", layer=8))

    # ---- the letter card: 1000 x 640 px of paper, twelve handwritten lines
    create("Letter", "LetterCard", pos=(0, 0.3, 0), Sprite=sprite("textures/ui/letter_card.png", size=px(1000, 640), layer=9, tint=(1, 1, 1, 0)))
    for i in range(12):
        create(f"Letter line {i}", "LetterLine", i, pos=(-6.6, 4.4 - i * 0.74, 0), TextLabel=label(HAND_FONT, 0.47, "Left", INK, layer=10))

    # ---- the panel: title menu backdrop, the journal, the backlog, the ending card
    create("Panel", "Panel", pos=(0, 0.2, 0), Sprite=sprite("textures/ui/panel.png", size=px(1100, 620), layer=9, tint=(1, 1, 1, 0)))
    for i in range(14):
        create(f"Panel line {i}", "PanelLine", i, pos=(-7.6, 4.3 - i * 0.62, 0), TextLabel=label(PROSE_FONT, 0.46, "Left", layer=10))

    # ---- the drawer: seven envelopes, a stamp over each delivered one
    def envelope_pos(i):
        return (-6.0 + 4.0 * i, 1.6) if i < 4 else (-4.0 + 4.0 * (i - 4), -1.6)

    for i in range(7):
        x, y = envelope_pos(i)
        create(f"Envelope {i + 1}", "Envelope", i, pos=(x, y, 0), Sprite=sprite("textures/ui/envelope.png", size=(3.4, 2.125), layer=3, tint=(1, 1, 1, 0)))
    for i in range(6):
        x, y = envelope_pos(i)
        create(f"Stamp {i + 1}", "Stamp", i, pos=(x + 0.6, y - 0.3, 0), rot=[0, 0, -0.1045, 0.9945],
               Sprite=sprite("textures/ui/stamp.png", size=(1.9, 0.86), layer=4, tint=(0.75, 0.2, 0.15, 0)))

    # ---- text that stands alone
    create("Hud", "Hud", pos=(9.6, 5.15, 0), TextLabel=label(PROSE_FONT, 0.38, "Right", [0.3, 0.24, 0.19, 0.85], layer=11))
    create("Title", "Title", pos=(0, 4.6, 0), TextLabel=label(BOLD_FONT, 1.25, "Center", [0.98, 0.94, 0.85, 1], layer=11))

    # ---- the two sound beds (the game swaps their clips)
    create("Music", "Music", AudioSource={"loop": True, "play_on_start": False, "bus": 1, "gain_db": -8.0})
    create("Ambience", "Ambience", AudioSource={"loop": True, "play_on_start": False, "bus": 2, "gain_db": -12.0})

    saved = m.tool("scene_save", path=args.scene, force=True)
    cook = m.tool("cook_run", adopt=True)
    run = m.tool("run_headless", ticks=args.ticks, log_lines=0)
    m.close()
    print(f"author_scene_mcp: {created} entities, {m.calls} MCP calls\nscene_save {json.dumps(saved)[:200]}\n"
          f"cook_run {json.dumps(cook)[:300]}\nrun_headless {json.dumps(run)[:400]}")


if __name__ == "__main__":
    main()
