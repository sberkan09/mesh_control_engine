#!/usr/bin/env python3
"""Kindle's new-mechanic art: the colour-switch post and the red / blue switch blocks (16x16, the
same tile size and PNG writer as scripts/new_project.py). Re-running it keeps the GUIDs of existing
sidecars, so scenes that reference the textures stay valid.

    python3 projects/Kindle/tools/gen_switch_art.py
"""

import pathlib
import struct
import uuid
import zlib

OUT = pathlib.Path(__file__).resolve().parents[1] / "assets" / "textures" / "switch"


def png(width, height, pixel):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixel(x, y))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def block(base, dark, light):
    """A bevelled block: dark rim, light top-left bevel, a recessed square in the middle."""
    def pixel(x, y):
        if x in (0, 15) or y in (0, 15):
            return dark + (255,)
        if x == 1 or y == 1:
            return light + (255,)
        if x == 14 or y == 14:
            return tuple(int(c * 0.8) for c in base) + (255,)
        if 5 <= x <= 10 and 5 <= y <= 10:
            inner = x in (5, 10) or y in (5, 10)
            return (dark if inner else tuple(int(c * 0.9) for c in base)) + (255,)
        return base + (255,)
    return pixel


def switch_pixel(x, y):
    """A pressure post: a near-white lamp (the game tints it with the active colour) on a dark base."""
    if y >= 12:
        return (60, 56, 64, 255) if 1 <= x <= 14 else (0, 0, 0, 0)
    if 6 <= x <= 9 and 7 <= y <= 11:
        return (150, 146, 156, 255)
    dx, dy = x - 7.5, y - 4.5
    if dx * dx + dy * dy <= 16.0:
        return (255, 255, 255, 255) if dx * dx + dy * dy > 4.0 or (x + y) % 3 else (230, 230, 236, 255)
    return (0, 0, 0, 0)


def write(name, pixel):
    path = OUT / name
    path.write_bytes(png(16, 16, pixel))
    side = path.with_name(path.name + ".mceasset")
    if not side.exists():
        side.write_text('{\n  "guid": "%s",\n  "importer": "texture",\n  "importer_version": 1,\n'
                        '  "settings": { "srgb": true, "mips": false }\n}\n' % uuid.uuid4(), encoding="utf-8", newline="\n")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    write("red_block.png", block((214, 64, 56), (110, 24, 26), (250, 140, 120)))
    write("blue_block.png", block((58, 112, 220), (20, 40, 110), (130, 180, 255)))
    write("switch.png", switch_pixel)
    print(f"gen_switch_art: 3 textures -> {OUT}")


if __name__ == "__main__":
    main()
