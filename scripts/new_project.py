#!/usr/bin/env python3
"""New-project generator (ROADMAP M8, assets.md §4.4): stamps templates/project into a project
that compiles, cooks and ships through the one-command pipeline.

    python3 scripts/new_project.py MyGame                  # -> projects/MyGame, game id my_game
    python3 scripts/new_project.py MyGame --game hero --title "My Game"
    python3 scripts/new_project.py Probe --dest build/x    # anywhere (the build's template probe)
    python3 scripts/new_project.py MyNovel --template novel  # a visual novel on mce_kit_novel
    python3 scripts/new_project.py MyScripts --template script  # Luau scripts and data: no C++, no compiler

The engine's CMake adds every projects/<name>/ directory, so after `cmake --preset <preset>` the
targets <game> (host), <game>_game (module) and <game>_cook exist. Ship with
`python3 scripts/ship.py --project projects/<name>`. GUIDs are fresh per project unless --seed
makes them reproducible. Exit code 0 = project written.
"""

import argparse
import os
import pathlib
import random
import re
import struct
import subprocess
import sys
import uuid
import zlib

# In the engine tree: scripts/new_project.py beside templates/project and CMakeLists.txt. Installed
# with the SDK: share/mce/scripts/new_project.py beside share/mce/templates/project and VERSION.
ROOT = pathlib.Path(__file__).resolve().parents[1]
TEMPLATES = ROOT / "templates"
# "script" stands alone (Luau scripts and data, no C++ and no CMake); any other is an overlay: templates/project first, then its own files over it.
TEMPLATE_CHOICES = ("project", "novel", "script")
IN_ENGINE_TREE = (ROOT / "CMakeLists.txt").is_file()
TEXT_SUFFIXES = {".txt", ".md", ".cpp", ".h", ".mceproj", ".mcscene", ".mceasset", ".gitignore", ".json", ".mcuitheme", ".mcui", ".mcsheet"}


def game_id(name):
    """MyGame -> my_game: a C++ namespace, a CMake target and a library file name."""
    s = re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", name)
    s = re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_").lower()
    if not s or s[0].isdigit():
        s = "game_" + s
    return s


def engine_version():
    if not IN_ENGINE_TREE:  # the SDK's VERSION: the version line, then the engine's build stamp
        version = ROOT / "VERSION"
        return version.read_text(encoding="utf-8").splitlines()[0].strip() if version.is_file() else "0.0.0"
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(MeshControlEngine[^)]*VERSION\s+([0-9.]+)", text, re.S)
    return m.group(1) if m else "0.0.0"


def png(width, height, pixel):
    """A tiny RGBA PNG; pixel(x, y) -> (r, g, b, a)."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixel(x, y))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def block_pixel(x, y):  # a warm tile with a darker rim
    edge = x in (0, 15) or y in (0, 15)
    return (150, 82, 40, 255) if edge else (232, 146, 62, 255)


def ground_pixel(x, y):  # grass on top, soil below
    if y < 4:
        return (88, 170, 74, 255) if (x + y) % 5 else (70, 140, 60, 255)
    return (110, 74, 46, 255) if (x * 7 + y * 3) % 11 else (92, 60, 38, 255)


def novel_wall_pixel(x, y):  # a dusk wall over a floorboard band
    if y >= 48:
        return (92, 64, 44, 255) if (x // 16 + y // 4) % 2 else (84, 58, 40, 255)
    t = y / 48.0
    return (int(70 + 60 * t), int(62 + 44 * t), int(88 + 20 * t), 255)


def novel_guide_pixel(x, y):  # a soft silhouette: a head over shoulders
    dx, head = x - 16, (x - 16) ** 2 + (y - 14) ** 2 <= 64
    body = y >= 24 and abs(dx) <= 6 + (y - 24) // 3
    return (236, 214, 190, 255) if head else (120, 92, 140, 255) if body else (0, 0, 0, 0)

# The starter themes' sprite skins (M-UI-DATA step 7): a 64 x 16 sheet per theme with a white
# block (region 0, flat quads: the middle of an 8 x 8 block, so linear filtering never reaches the
# transparent texels around it), a panel (region 1, x 16) and a button (region 2, x 40), each
# 16 x 16 with a gap between them and drawn in greys so the theme's palette tints them. The
# .mcsheet beside each names the regions.
SKIN_SIZE = (64, 16)


def rounded_alpha(x, y, radius):
    """Coverage of a 16 x 16 cell's rounded rect at pixel (x, y), 4 x 4 supersampled."""
    hits = 0
    for sy in range(4):
        for sx in range(4):
            px, py = x + (sx + 0.5) / 4, y + (sy + 0.5) / 4
            cx = min(max(px, radius), 16 - radius)
            cy = min(max(py, radius), 16 - radius)
            hits += (px - cx) ** 2 + (py - cy) ** 2 <= radius * radius
    return hits / 16


def pixel_cell(kind, x, y):
    edge = x in (0, 15) or y in (0, 15)
    if kind == "button":
        if (x in (0, 15)) and (y in (0, 15)):
            return (0, 0, 0, 0)  # notched corners
        if edge:
            return (24, 22, 30, 255)
        if y == 1 or (x == 1 and y < 14):
            return (255, 255, 255, 255)
        if y >= 13 or x == 14:
            return (150, 150, 150, 255)
        return (225, 225, 225, 255)
    if edge:
        return (24, 22, 30, 255)
    if x == 1 or y == 1:
        return (255, 255, 255, 255)
    if x == 14 or y == 14:
        return (180, 180, 180, 255)
    return (255, 255, 255, 255)


def clean_cell(kind, x, y):
    a = rounded_alpha(x, y, 6.0 if kind == "panel" else 5.0)
    if kind == "button" and y >= 14:
        return (205, 205, 205, int(255 * a))  # a soft bottom lip
    return (255, 255, 255, int(255 * a))


def story_cell(kind, x, y):
    a = rounded_alpha(x, y, 3.0)
    inner = min(x, y, 15 - x, 15 - y)
    if kind == "panel":
        if inner == 0:
            v = 120  # an ink rule
        elif inner == 3 and 3 <= x <= 12 and 3 <= y <= 12:
            v = 190  # a second, finer rule inside it
        else:
            v = 255  # the middle stretches, so it stays flat
        return (v, v, v, int(255 * a))
    if inner == 0:
        v = 110
    elif y == 1:
        v = 255
    elif y >= 13:
        v = 185
    else:
        v = 232
    return (v, v, v, int(255 * a))


SKIN_CELLS = {"PIXEL": pixel_cell, "CLEAN": clean_cell, "STORY": story_cell}


def skin_pixel(theme):
    cell = SKIN_CELLS[theme]

    def pixel(x, y):
        if x < 16:
            return (255, 255, 255, 255) if x < 8 and y < 8 else (0, 0, 0, 0)
        if 16 <= x < 32:
            return cell("panel", x - 16, y)
        if 40 <= x < 56:
            return cell("button", x - 40, y)
        return (0, 0, 0, 0)

    return pixel


def texture_sidecar(guid, filter_mode):
    return ('{\n  "guid": "%s",\n  "importer": "texture",\n  "importer_version": 1,\n'
            '  "settings": { "srgb": true, "mips": false, "filter": "%s" }\n}\n' % (guid, filter_mode))


def story_compiler():
    """tools/story/story_compile.py in the engine tree, beside this script in the SDK."""
    for path in (ROOT / "tools" / "story" / "story_compile.py", pathlib.Path(__file__).resolve().parent / "story_compile.py"):
        if path.is_file():
            return path
    return None


def sidecar(guid):
    return ('{\n  "guid": "%s",\n  "importer": "texture",\n  "importer_version": 1,\n'
            '  "settings": { "srgb": true, "mips": false }\n}\n' % guid)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", help="project name, e.g. MyGame (letters, digits, _ and -)")
    ap.add_argument("--game", help="game id (default: the name in snake_case)")
    ap.add_argument("--title", help="window title (default: the name)")
    ap.add_argument("--template", choices=TEMPLATE_CHOICES, default="project",
                    help="project: the 2D starter (default); novel: a visual novel on the novel kit; "
                         "script: a game of Luau scripts and data, no C++ and no compiler")
    ap.add_argument("--dest", default=str(ROOT / "projects") if IN_ENGINE_TREE else os.getcwd(),
                    help="parent directory (default: projects/ in the engine tree, the current directory with the SDK)")
    ap.add_argument("--seed", type=int, help="derive the GUIDs from this seed (reproducible output)")
    ap.add_argument("--force", action="store_true", help="overwrite an existing project directory")
    args = ap.parse_args()

    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", args.name):
        ap.error("the name must start with a letter and hold only letters, digits, _ and -")
    game = args.game or game_id(args.name)
    if not re.fullmatch(r"[a-z][a-z0-9_]*", game):
        ap.error("the game id must be snake_case (a C++ namespace and a library name)")
    out = pathlib.Path(args.dest).resolve() / args.name
    if out.exists() and any(out.iterdir()) and not args.force:
        print(f"new_project: {out} exists (use --force to overwrite)", file=sys.stderr)
        return 1

    rng = random.Random(args.seed) if args.seed is not None else None

    def new_guid():
        return str(uuid.UUID(int=rng.getrandbits(128), version=4)) if rng else str(uuid.uuid4())

    tokens = {
        "@PROJECT@": args.name,
        "@GAME@": game,
        "@TITLE@": args.title or args.name,
        "@ENGINE_VERSION@": engine_version(),
        "@GUID_SCENE@": new_guid(),
        "@GUID_TEX_BLOCK@": new_guid(),
        "@GUID_TEX_GROUND@": new_guid(),
        "@GUID_FONT@": new_guid(),
        "@GUID_SCRIPT@": new_guid(),
        "@GUID_UI_OPTIONS@": new_guid(),  # assets/ui/options.mcui, the stock options screen (M-ACCESS)
    }
    # The starter themes (M-UI-DATA step 7): assets/ui/themes/{pixel,clean,story}.mcuitheme and their fonts.
    for theme in ("PIXEL", "CLEAN", "STORY"):
        for token in (f"@GUID_THEME_{theme}@", f"@GUID_FONT_{theme}@", f"@GUID_FONT_{theme}_TITLE@", f"@GUID_SKIN_{theme}@",
                      f"@GUID_SKIN_TEX_{theme}@"):
            tokens[token] = new_guid()

    def stamp(text):
        for k, v in tokens.items():
            text = text.replace(k, v)
        return text

    files = {}  # relative path -> source; an overlay's file replaces the base template's
    script_project = args.template == "script"
    bases = ["script"] if script_project else ["project"] + ([args.template] if args.template != "project" else [])
    for template in bases:
        root = TEMPLATES / template
        if not root.is_dir():
            print(f"new_project: the {template} template is missing ({root})", file=sys.stderr)
            return 1
        for src in sorted(root.rglob("*")):
            if src.is_file():
                files[src.relative_to(root).as_posix()] = src
    written = 0
    for rel, src in sorted(files.items()):
        dst = out / stamp(rel)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.suffix in TEXT_SUFFIXES or src.name in TEXT_SUFFIXES or src.name == "CMakeLists.txt" or src.suffix == ".story":
            dst.write_text(stamp(src.read_text(encoding="utf-8")), encoding="utf-8", newline="\n")
        else:
            dst.write_bytes(src.read_bytes())
        written += 1

    tex = out / "assets" / "textures"
    if script_project:
        tex.mkdir(parents=True, exist_ok=True)
        (tex / "block.png").write_bytes(png(16, 16, block_pixel))
        (tex / "block.png.mceasset").write_text(sidecar(tokens["@GUID_TEX_BLOCK@"]), encoding="utf-8", newline="\n")
        written += 2
        print(f"new_project: {args.name} (a script game) -> {out} ({written} files)")
        if IN_ENGINE_TREE:
            print(f"  cook: build/<preset>/tools/cook/mce_cook {out} --pack    play: mce_player -pack {out}/assets.mce")
        else:
            print(f"  build: mce build {out}    play: mce play {out}    edit: mce editor {out}")
        return 0
    tex.mkdir(parents=True, exist_ok=True)
    if args.template == "novel":
        # The novel's art by the kit's path conventions: a background layer and the guide's sprite.
        (tex / "bg").mkdir(exist_ok=True)
        (tex / "cast").mkdir(exist_ok=True)
        art = [("bg/room_0_wall.png", png(64, 64, novel_wall_pixel), "@GUID_TEX_BLOCK@"),
               ("cast/guide_neutral_stand.png", png(32, 64, novel_guide_pixel), "@GUID_TEX_GROUND@")]
    else:
        art = [("block.png", png(16, 16, block_pixel), "@GUID_TEX_BLOCK@"),
               ("ground.png", png(16, 16, ground_pixel), "@GUID_TEX_GROUND@")]
    for rel, data, guid in art:
        (tex / rel).write_bytes(data)
        (tex / f"{rel}.mceasset").write_text(sidecar(tokens[guid]), encoding="utf-8", newline="\n")
        written += 2

    # The starter themes' skin textures (their .mcsheet files come from the template).
    themes = out / "assets" / "ui" / "themes"
    for theme in SKIN_CELLS:
        name = f"{theme.lower()}_skin.png"
        (themes / name).write_bytes(png(*SKIN_SIZE, skin_pixel(theme)))
        (themes / f"{name}.mceasset").write_text(texture_sidecar(tokens[f"@GUID_SKIN_TEX_{theme}@"],
                                                                 "nearest" if theme == "PIXEL" else "linear"),
                                                 encoding="utf-8", newline="\n")
        written += 2

    if args.template == "novel":
        # The story header, so the project builds before its first edit (the build recompiles it).
        compiler = story_compiler()
        if compiler is None:
            print("new_project: story_compile.py is missing; run mce story compile in the project", file=sys.stderr)
            return 1
        rc = subprocess.run([sys.executable, str(compiler), str(out)], stdout=subprocess.DEVNULL).returncode
        if rc != 0:
            print(f"new_project: the template's story did not compile (exit {rc})", file=sys.stderr)
            return 1
        written += 2

    if IN_ENGINE_TREE:
        rel_out = os.path.relpath(out, ROOT)
        print(f"new_project: {args.name} (game '{game}') -> {rel_out} ({written} files)")
        print(f"  build: cmake --preset <preset> && cmake --build --preset <preset> --target {game}")
        print(f"  ship:  python3 scripts/ship.py --project {rel_out}")
    else:
        print(f"new_project: {args.name} (game '{game}') -> {out} ({written} files)")
        print(f"  build: mce build {out}    play: mce play {out}    edit: mce editor {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
