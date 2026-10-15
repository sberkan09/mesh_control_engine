#!/usr/bin/env python3
"""Gives Undertow a data-driven UI through the project's MCP server (the M-UI-DATA demo, SBA-38).

The record of how assets/ui/ and the UI entities of scenes/main.mcscene and attract.mcscene were
made: an MCP client (this script, run by an agent) speaks JSON-RPC to `undertow_mcp --project
projects/Undertow` and only calls tools. No game or host code is involved: the screens are .mcui
documents on UiScreen entities, their navigation is kit::UiFlow data (show, hide and hold on input
actions), the HUD binds Drone and RunStats fields, and the options sliders bind the audio convars.

    python3 projects/Undertow/tools/author_ui_mcp.py --mcp build/linux-gcc-debug/projects/Undertow/undertow_mcp

Run it after tools/author_level_mcp.py, which rebuilds both scenes from scratch. Running it twice
keeps the assets' GUIDs (the documents and themes are rewritten in place) and replaces the UI
entities.

    main.mcscene     a title menu (holds the game until PLAY), options with two bound sliders, a
                     pause modal (Esc: RESUME, OPTIONS, QUIT TO TITLE) and the HUD, in the "deep" theme
    attract.mcscene  the same HUD in the "surface" theme: one document, two looks
"""

import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

FONTS = {  # library id -> (file under assets/fonts/, baked size)
    "font-pixelify-sans": ("PixelifySans.ttf", 22),
    "font-press-start-2p": ("PressStart2P-Regular.ttf", 30),
    "font-atkinson-hyperlegible-next": ("AtkinsonHyperlegibleNext.ttf", 24),
    "font-karla": ("Karla.ttf", 40),
}

THEMES = {
    "ui/deep.mcuitheme": {
        "palette": {
            "panel": [0.02, 0.1, 0.14, 0.92],
            "button": [0.05, 0.4, 0.46, 1], "button_hover": [0.1, 0.54, 0.6, 1], "button_pressed": [0.03, 0.26, 0.3, 1],
            "button_focused": [1, 0.74, 0.24, 1], "button_disabled": [0.12, 0.2, 0.24, 1],
            "text": [0.86, 1, 1, 1], "text_focused": [0.04, 0.08, 0.1, 1], "label": [0.55, 0.95, 0.95, 1],
            "accent": [1, 0.74, 0.24, 1],
        },
        "fonts": ("font-pixelify-sans", "font-press-start-2p", "font-pixelify-sans"),
        "styles": {"Text": {"style": {"text_shadow": [2, 2], "text_shadow_color": [0, 0, 0, 1]}}},
    },
    "ui/surface.mcuitheme": {
        "palette": {
            "panel": [0.97, 0.91, 0.8, 0.94],
            "button": [0.9, 0.42, 0.2, 1], "button_hover": [0.98, 0.52, 0.28, 1], "button_pressed": [0.66, 0.28, 0.12, 1],
            "button_focused": [0.16, 0.32, 0.62, 1], "button_disabled": [0.75, 0.68, 0.6, 1],
            "text": [1, 0.98, 0.94, 1], "text_focused": [1, 1, 1, 1], "label": [1, 0.86, 0.6, 1],
            "accent": [0.95, 0.35, 0.55, 1],
        },
        "fonts": ("font-atkinson-hyperlegible-next", "font-karla", "font-atkinson-hyperlegible-next"),
        "styles": {"Text": {"style": {"text_outline": 1, "text_outline_color": [0.2, 0.1, 0.05, 1]}}},
    },
}


def button(name, text, action, up=None, down=None, focus=False):
    w = {"name": name, "kind": "Button", "text": text, "action": action}
    nav = {k: v for k, v in (("up", up), ("down", down)) if v}
    if nav:
        w["nav"] = nav
    if focus:
        w["default_focus"] = True
    return w


def menu(veil_action, title, items, tint):
    """A modal veil over the game with a centred column: `title` then `items`."""
    veil = {"name": "veil", "fill": True, "blocks_pointer": True, "style": {"tint": tint},
            "children": [{"name": "menu", "kind": "Stack", "at": [0.5, 0.5, 380, 0], "stack": {"spacing": 14},
                          "children": [{"name": "title", "kind": "Text", "text": title, "font_role": "Title"}] + items}]}
    if veil_action:
        veil["action"] = veil_action  # Cancel (Esc, the gamepad's B) on the open modal presses it
    return [veil]


M = "veil/menu/"
DOCS = {
    "ui/title.mcui": (True, menu(None, "UNDERTOW", [
        {"name": "tagline", "kind": "Text", "text": "HAUL FIVE REACTOR CORES HOME"},
        button("play", "PLAY", "Play", down=M + "options", focus=True),
        button("options", "OPTIONS", "Options", up=M + "play"),
    ], [0, 0.03, 0.06, 0.8])),
    "ui/options.mcui": (True, menu("Back", "OPTIONS", [
        {"name": "music_label", "kind": "Text", "text": "MUSIC"},
        {"name": "music", "kind": "Slider", "range": [-40, 0, 2], "convar": "audio.music_db", "default_focus": True,
         "nav": {"down": M + "sfx"}},
        {"name": "sfx_label", "kind": "Text", "text": "EFFECTS"},
        {"name": "sfx", "kind": "Slider", "range": [-40, 0, 2], "convar": "audio.sfx_db",
         "nav": {"up": M + "music", "down": M + "back"}},
        button("back", "BACK", "Back", up=M + "sfx"),
    ], [0, 0.02, 0.04, 0.6])),
    "ui/pause.mcui": (True, menu("Resume", "PAUSED", [
        button("resume", "RESUME", "Resume", down=M + "options", focus=True),
        button("options", "OPTIONS", "Options", up=M + "resume", down=M + "quit"),
        button("quit", "QUIT TO TITLE", "ToTitle", up=M + "options"),
    ], [0, 0.02, 0.04, 0.6])),
    "ui/hud.mcui": (False, [
        {"name": "stats", "kind": "Stack", "stack": {"spacing": 4},
         "anchors": {"min": [0, 0], "max": [0, 0], "offset_min": [20, 16], "offset_max": [280, 0], "to": "SafeArea"},
         "children": [
             {"name": "hull_label", "kind": "Text", "text": "HULL"},
             {"name": "hull", "kind": "Slider", "range": [0, 3, 1], "bind": "Drone/hull", "interactable": False},
             {"name": "energy_label", "kind": "Text", "text": "ENERGY"},
             {"name": "energy", "kind": "Slider", "range": [0, 100, 1], "bind": "Drone/energy", "interactable": False},
             {"name": "cores", "kind": "Text", "bind": "RunStats/cores", "format": "CORES %g / 5"},
         ]},
    ]),
}


class Mcp:
    def __init__(self, exe, project, transcript):
        self.proc = subprocess.Popen([exe, "--project", str(project)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.next_id = 1
        self.calls = 0
        self.log = open(transcript, "w", encoding="utf-8") if transcript else None
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "undertow-ui-author", "version": "1"}})
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
                raise RuntimeError(f"undertow_mcp closed the pipe during {method}")
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
            raise RuntimeError(f"{tool_name}({json.dumps(args)[:300]}): {text}")
        return json.loads(text) if text.startswith(("{", "[")) else text

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=30)
        if self.log:
            self.log.close()


def guids(m):
    return {a["path"]: a["guid"] for a in m.tool("asset_list")["assets"]}


def ref(g):  # asset_list already names GUIDs "guid:<uuid>"
    return g if g.startswith("guid:") else f"guid:{g}"


def ui_entities(m, scene, screens):
    """Opens `scene`, replaces its UI entities (named "UI ...") and the old text HUD line, saves it."""
    m.tool("scene_open", path=scene, force=True)
    names = {e["name"] for e in m.tool("scene_list_entities")["entities"]}
    for name in sorted(names):
        if name.startswith("UI ") or name == "HUD Status":  # the HUD document shows hull, energy and cores now
            m.tool("entity_delete", entity=name)
    for name, components in screens:
        m.tool("entity_create", name=name, transform=False, components=components)
    m.tool("scene_save")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mcp", required=True, help="the project's MCP server (undertow_mcp)")
    ap.add_argument("--transcript", help="write every JSON-RPC line here")
    ap.add_argument("--shots", help="also save ui_preview PNGs of the documents under this project folder (e.g. shots/)")
    args = ap.parse_args()
    m = Mcp(args.mcp, ROOT, args.transcript)

    # The fonts: from the engine's content library (license files and CREDITS.md rows come along),
    # baked with Latin-1 and Latin Extended-A at the size each one is used at.
    have = guids(m)
    for lib_id, (file, size) in FONTS.items():
        if f"fonts/{file}" not in have:
            m.tool("library_add", id=lib_id, path="fonts/")
        m.tool("asset_set_import_settings", path=f"fonts/{file}",
               settings={"size": size, "ranges": [[32, 126], [160, 383]], "padding": 1})
    have = guids(m)
    font_guid = {lib_id: have[f"fonts/{file}"] for lib_id, (file, _) in FONTS.items()}

    # Two themes: the same documents look like a deep-sea console or a sunlit surface.
    for path, t in THEMES.items():
        body, title, btn = (ref(font_guid[f]) for f in t["fonts"])
        content = {"palette": t["palette"], "fonts": {"body": body, "title": title, "button": btn}, "styles": t["styles"]}
        if path in have:  # a second run rewrites it under its GUID
            m.tool("asset_write", path=path, content=content)
        else:
            m.tool("asset_create", path=path, content=content)

    # The documents (ui_create checks each with the cook before writing it).
    for path, (modal, widgets) in DOCS.items():
        if path in have:  # a second run rewrites the document under its GUID
            m.tool("asset_write", path=path, content={"modal": modal, "widgets": widgets})
        else:
            m.tool("ui_create", path=path, modal=modal, widgets=widgets)
    have = guids(m)
    doc = {p: ref(have[p]) for p in DOCS}
    deep, surface = ref(have["ui/deep.mcuitheme"]), ref(have["ui/surface.mcuitheme"])
    for path in DOCS:  # the documents' own look (ui_preview draws it); a UiScreen's theme still wins
        m.tool("ui_theme_set", document=path, theme=deep)
    main_scene = ref(have["scenes/main.mcscene"])

    def screen(document, theme, order, visible=True, **flow):
        c = {"UiScreen": {"document": doc[document], "theme": theme, "order": order, "visible": visible}}
        if flow:
            c["UiFlow"] = flow
        return c

    ui_entities(m, "scenes/main.mcscene", [
        ("UI Hud", screen("ui/hud.mcui", deep, 0)),
        ("UI Pause", screen("ui/pause.mcui", deep, 10, False, show_on="Pause", hide_on="Resume", load_on="ToTitle",
                            scene=main_scene, pause=True)),
        ("UI Title", screen("ui/title.mcui", deep, 20, hide_on="Play", pause=True)),
        ("UI Options", screen("ui/options.mcui", deep, 30, False, show_on="Options", hide_on="Back")),
    ])
    ui_entities(m, "scenes/attract.mcscene", [("UI Hud", screen("ui/hud.mcui", surface, 0))])

    cook = m.tool("cook_run", pack=True)
    if not cook["ok"]:
        print(json.dumps(m.tool("cook_errors"), indent=1)[:3000])
        sys.exit("author_ui_mcp: the cook failed")

    # The game holds behind the title until PLAY, Esc pauses it, RESUME lets it go: the same
    # scripted run twice gives the same hash.
    script = "5 Play\n30-90 Move=1,0\n100 Pause\n140 Resume\n150-200 Move=0,1"
    runs = [m.tool("run_headless", scene="scenes/main.mcscene", ticks=240, input=script) for _ in range(2)]
    if runs[0].get("state_hash") != runs[1].get("state_hash"):
        sys.exit(f"author_ui_mcp: the scripted main run is not deterministic: {runs[0].get('state_hash')} vs {runs[1].get('state_hash')}")
    attract = m.tool("run_headless", scene="scenes/attract.mcscene", ticks=600)

    if args.shots:
        for path in DOCS:
            m.tool("ui_preview", document=path, save=f"{args.shots.rstrip('/')}/{pathlib.Path(path).stem}.png")
        m.tool("ui_theme_set", document="ui/hud.mcui", theme=surface)  # the attract scene's look
        m.tool("ui_preview", document="ui/hud.mcui", save=f"{args.shots.rstrip('/')}/hud_surface.png")
        m.tool("ui_theme_set", document="ui/hud.mcui", theme=deep)
    print(f"author_ui_mcp: {m.calls} tool calls; main run hash {runs[0].get('state_hash')}, attract {attract.get('state_hash')}")
    m.close()


if __name__ == "__main__":
    main()
