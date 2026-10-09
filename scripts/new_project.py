#!/usr/bin/env python3
"""New-project generator (ROADMAP M8, assets.md §4.4): stamps templates/project into a project
that compiles, cooks and ships through the one-command pipeline.

    python3 scripts/new_project.py MyGame                  # -> projects/MyGame, game id my_game
    python3 scripts/new_project.py MyGame --game hero --title "My Game"
    python3 scripts/new_project.py Probe --dest build/x    # anywhere (the build's template probe)

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
import sys
import uuid
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "templates" / "project"
TEXT_SUFFIXES = {".txt", ".md", ".cpp", ".h", ".mceproj", ".mcscene", ".gitignore", ".json"}


def game_id(name):
    """MyGame -> my_game: a C++ namespace, a CMake target and a library file name."""
    s = re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", name)
    s = re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_").lower()
    if not s or s[0].isdigit():
        s = "game_" + s
    return s


def engine_version():
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


def sidecar(guid):
    return ('{\n  "guid": "%s",\n  "importer": "texture",\n  "importer_version": 1,\n'
            '  "settings": { "srgb": true, "mips": false }\n}\n' % guid)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", help="project name, e.g. MyGame (letters, digits, _ and -)")
    ap.add_argument("--game", help="game id (default: the name in snake_case)")
    ap.add_argument("--title", help="window title (default: the name)")
    ap.add_argument("--dest", default=str(ROOT / "projects"), help="parent directory (default: projects/)")
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
    }

    def stamp(text):
        for k, v in tokens.items():
            text = text.replace(k, v)
        return text

    written = 0
    for src in sorted(TEMPLATE.rglob("*")):
        if not src.is_file():
            continue
        rel = stamp(src.relative_to(TEMPLATE).as_posix())
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.suffix in TEXT_SUFFIXES or src.name in TEXT_SUFFIXES or src.name == "CMakeLists.txt":
            dst.write_text(stamp(src.read_text(encoding="utf-8")), encoding="utf-8", newline="\n")
        else:
            dst.write_bytes(src.read_bytes())
        written += 1

    tex = out / "assets" / "textures"
    tex.mkdir(parents=True, exist_ok=True)
    (tex / "block.png").write_bytes(png(16, 16, block_pixel))
    (tex / "block.png.mceasset").write_text(sidecar(tokens["@GUID_TEX_BLOCK@"]), encoding="utf-8", newline="\n")
    (tex / "ground.png").write_bytes(png(16, 16, ground_pixel))
    (tex / "ground.png.mceasset").write_text(sidecar(tokens["@GUID_TEX_GROUND@"]), encoding="utf-8", newline="\n")
    written += 4

    rel_out = os.path.relpath(out, ROOT)
    print(f"new_project: {args.name} (game '{game}') -> {rel_out} ({written} files)")
    print(f"  build: cmake --preset <preset> && cmake --build --preset <preset> --target {game}")
    print(f"  ship:  python3 scripts/ship.py --project {rel_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
