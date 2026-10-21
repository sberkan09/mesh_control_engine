#!/usr/bin/env python3
"""Draws Prism Break's sprites (tiny RGBA PNGs, standard library only) into assets/textures/.

    python3 projects/PrismBreak/tools/gen_art.py

Bricks and the ball are drawn near-white so the game's Sprite tint colours them. The output is
committed; re-run after changing a drawing. New files need a .mceasset sidecar (the cook's
--adopt writes one).
"""

import math
import random
import pathlib
import struct
import zlib

OUT = pathlib.Path(__file__).resolve().parents[1] / "assets" / "textures"


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


def brick(x, y):  # 32 x 16, bevelled: light top-left rim, dark bottom-right rim
    w, h = 32, 16
    if x == 0 or y == 0:
        return (255, 255, 255, 255)
    if x == w - 1 or y == h - 1:
        return (120, 120, 130, 255)
    if x == 1 or y == 1:
        return (245, 245, 250, 255)
    if x == w - 2 or y == h - 2:
        return (170, 170, 180, 255)
    shine = 20 if (x + y) % 9 == 0 and y < 6 else 0
    return (215 + shine, 215 + shine, 225 + shine // 2, 255)


def ball(x, y):  # 16 x 16 disc with a highlight, soft edge
    cx = cy = 7.5
    d = math.hypot(x - cx, y - cy)
    if d > 7.5:
        return (0, 0, 0, 0)
    a = 255 if d < 6.8 else int(255 * (7.5 - d) / 0.7)
    hl = math.hypot(x - 5, y - 5)
    v = 255 if hl < 2.2 else 235 - int(d * 6)
    return (v, v, min(255, v + 10), a)


def paddle(x, y):  # 64 x 12, a rounded capsule, teal with a light top edge
    w, h = 64, 12
    r = h / 2
    cx = min(max(x + 0.5, r), w - r)
    d = math.hypot(x + 0.5 - cx, y + 0.5 - r)
    if d > r:
        return (0, 0, 0, 0)
    if y <= 2:
        return (190, 255, 245, 255)
    if y >= h - 3:
        return (20, 110, 120, 255)
    return (60, 200, 200, 255)


def wall(x, y):  # 16 x 16 dark stone
    edge = x in (0, 15) or y in (0, 15)
    n = (x * 13 + y * 7) % 5
    return (46, 50, 66, 255) if edge else (62 + n * 2, 68 + n * 2, 88 + n * 2, 255)


def backdrop(x, y):  # 320 x 180 night-sky gradient with sparse, faint one-texel stars
    t = y / 179
    r, g, b = int(14 + 18 * t), int(14 + 10 * t), int(34 + 30 * t)
    k = STARS.get((x, y))
    if k:
        r, g, b = r + k, g + k, b + k
    return (r, g, b, 255)


_rng = random.Random(7)
STARS = {(_rng.randrange(320), _rng.randrange(180)): _rng.randrange(40, 130) for _ in range(140)}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (w, h, fn) in {"brick": (32, 16, brick), "ball": (16, 16, ball), "paddle": (64, 12, paddle),
                             "wall": (16, 16, wall), "backdrop": (320, 180, backdrop)}.items():
        (OUT / f"{name}.png").write_bytes(png(w, h, fn))
        print("wrote", OUT / f"{name}.png")


if __name__ == "__main__":
    main()
