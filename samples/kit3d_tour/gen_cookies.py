#!/usr/bin/env python3
"""Writes the two light-cookie textures of the kit3d_tour cookie scene (M-LIGHTCOOKIE 40h) into assets/textures/.

    python3 samples/kit3d_tour/gen_cookies.py

Stdlib only and deterministic (a fixed seed, no clock, no platform maths beyond sin/cos), so every run writes the same bytes and
no binary is hand-drawn. The files and their sidecars are original to this repo (CC0-1.0); there is nothing to credit.

  cookie_window.png  256 x 256, the spot light's cookie: a window frame with 3 x 4 panes. Light passes the panes (white), the
                     mullions and frame are dark, the outside is black. It is asymmetric on purpose (a wide sill, a taller top
                     row, a small latch notch on the right) so a flipped u or v shows.
  cookie_leaves.png  256 x 256, the sun's cookie: a leaf dapple that tiles without a seam (every blob wraps around the border),
                     dark canopy shade with bright gaps. One repeat covers SunCookie3D::size metres.

A cookie multiplies the light in linear light and the PNGs are decoded as sRGB, so a grey texel is darker than it looks:
the dapple's shade is 0.35 (about 0.10 linear), its gaps 1.0.
"""
import math
import os
import random
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "assets", "textures")
N = 256
AA = 4  # supersampling per axis for the window


def write_png(path, width, height, rows):
    """rows: height lists of width (r, g, b) byte tuples. 8-bit RGB, one IDAT, filter 0."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for px in row:
            raw.extend(px)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def write_sidecar(path, guid):
    with open(path + ".mceasset", "w") as f:
        f.write('{\n "guid": "%s",\n "importer": "texture",\n "importer_version": 1,\n "settings": {}\n}\n' % guid)


def clamp01(v):
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


def smoothstep(a, b, x):
    t = clamp01((x - a) / (b - a))
    return t * t * (3.0 - 2.0 * t)


def window_light(u, v):
    """Light passing at (u, v) in 0..1, v down the image: 1 in a pane, 0 on frame, mullion and outside."""
    x0, x1 = 0.10, 0.90   # outer frame
    y0, y1 = 0.06, 0.94
    if not (x0 <= u <= x1 and y0 <= v <= y1):
        return 0.0
    frame = 0.035
    sill = 0.085          # wide sill at the bottom
    px0, px1 = x0 + frame, x1 - frame
    py0, py1 = y0 + frame, y1 - sill
    if not (px0 <= u <= px1 and py0 <= v <= py1):
        return 0.0
    mull = 0.022
    cols, rows = 3, 4
    # taller top row: row heights 1.4 : 1 : 1 : 1
    heights = [1.4, 1.0, 1.0, 1.0]
    total = sum(heights)
    # mullions between panes
    for i in range(1, cols):
        cx = px0 + (px1 - px0) * i / cols
        if abs(u - cx) < mull * 0.5:
            return 0.0
    acc = 0.0
    for h in heights[:-1]:
        acc += h
        cy = py0 + (py1 - py0) * acc / total
        if abs(v - cy) < mull * 0.5:
            return 0.0
    # latch notch on the right frame side, inside the last column's middle rows
    if u > px1 - 0.05 and 0.48 < v < 0.56:
        return 0.0
    return 1.0


def window():
    rows = []
    for y in range(N):
        row = []
        for x in range(N):
            acc = 0.0
            for j in range(AA):
                for i in range(AA):
                    acc += window_light((x + (i + 0.5) / AA) / N, (y + (j + 0.5) / AA) / N)
            g = int(acc / (AA * AA) * 255.0 + 0.5)
            row.append((g, g, g))
        rows.append(row)
    return rows


def leaves():
    rng = random.Random(20260411)
    blobs = []
    for _ in range(150):
        blobs.append((rng.random(), rng.random(), rng.uniform(0.025, 0.07), rng.uniform(0.35, 0.8), rng.uniform(0.0, math.pi)))
    # a leaf is an ellipse: radius r along its axis, r * aspect across; the smooth edge makes the light soft
    shade, gap = 0.35, 1.0
    rows = []
    for y in range(N):
        row = []
        for x in range(N):
            u, v = (x + 0.5) / N, (y + 0.5) / N
            cover = 0.0   # how much of the shade the leaves leave open here (the gaps are where the canopy is open)
            for bx, by, r, aspect, ang in blobs:
                du = (u - bx + 0.5) % 1.0 - 0.5   # wrapped, so the pattern tiles
                dv = (v - by + 0.5) % 1.0 - 0.5
                c, s = math.cos(ang), math.sin(ang)
                a = (du * c + dv * s) / r
                b = (-du * s + dv * c) / (r * aspect)
                d = math.sqrt(a * a + b * b)
                cover = max(cover, 1.0 - smoothstep(0.55, 1.0, d))
            t = cover
            g = shade + (gap - shade) * t
            q = int(g * 255.0 + 0.5)
            # a faint warm tint in the gaps, so the dapple is not grey on grass
            row.append((q, min(255, q), int(q * (0.88 + 0.12 * (1.0 - t)) + 0.5)))
        rows.append(row)
    return rows


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, guid, rows in (("cookie_window", "b0a30000-0000-4000-8000-000000000030", window()),
                             ("cookie_leaves", "b0a30000-0000-4000-8000-000000000031", leaves())):
        path = os.path.join(OUT, name + ".png")
        write_png(path, N, N, rows)
        write_sidecar(path, guid)
        print("wrote", os.path.relpath(path, HERE))


if __name__ == "__main__":
    main()
