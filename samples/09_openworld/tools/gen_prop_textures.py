#!/usr/bin/env python3
"""Writes the textures of the 09_openworld trees and bushes into assets/models/props/Textures/.

    python3 samples/09_openworld/tools/gen_prop_textures.py          (needs numpy and Pillow)

leaf_cluster.png   RGBA 256 x 256: a cluster of leaves (alpha is the cut-out, the colour under it is filled in so mip levels do not halo)
pine_bough.png     RGBA 256 x 256: a pine bough, the stem along the bottom-to-top middle, needles to both sides
bark_color.png     RGB 256 x 256, tileable: vertical bark ridges
bark_normal.png    the matching tangent-space normal map (import with srgb false)

Drawn here, seeded, so every run writes the same files; original to this repo (CC0). The sidecars are written by gen_assets.py.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_textures import fractal, normal_map  # noqa: E402

OUT = os.path.normpath(os.path.join(HERE, "..", "assets", "models", "props", "Textures"))
SS = 2          # supersampling for the drawn textures
S = 256


def finish(img, name):
    """Downsamples the supersampled RGBA image, hardens the alpha for a cut-out and floods the colour into the transparent texels."""
    a = np.asarray(img.resize((S, S), Image.LANCZOS)).astype(np.float32) / 255.0
    rgb, alpha = a[..., :3], a[..., 3]
    solid = alpha > 0.5
    # The colour of the transparent texels: the mean of the opaque ones, so a mip or a bilinear tap never pulls in black.
    mean = rgb[solid].mean(axis=0) if solid.any() else np.array([0.2, 0.4, 0.15])
    rgb = np.where(solid[..., None], rgb, mean)
    out = np.dstack([rgb, np.where(solid, 1.0, 0.0)])
    Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(OUT, name))


def leaf(draw, cx, cy, length, width, angle, fill, vein):
    """One pointed leaf: two arcs from the stalk to the tip, with a darker midrib."""
    ca, sa = math.cos(angle), math.sin(angle)
    pts = []
    for t in np.linspace(0, 1, 9):
        w = width / 2 * math.sin(math.pi * t ** 0.8)
        pts.append((t * length, w))
    pts += [(x, -w) for x, w in reversed(pts)]
    poly = [(cx + x * ca - y * sa, cy + x * sa + y * ca) for x, y in pts]
    draw.polygon(poly, fill=fill)
    draw.line([(cx, cy), (cx + length * 0.92 * ca, cy + length * 0.92 * sa)], fill=vein, width=max(1, int(width * 0.07)))


def leaf_cluster():
    rng = np.random.default_rng(101)
    n = S * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    centre = n / 2
    leaves = []
    for _ in range(95):
        r = (rng.random() ** 0.6) * n * 0.36
        a = rng.uniform(0, 2 * math.pi)
        leaves.append((r, a))
    leaves.sort(key=lambda p: p[0])           # inner leaves first, outer ones drawn over them
    for r, a in leaves:
        x, y = centre + r * math.cos(a), centre + r * math.sin(a)
        k = r / (n * 0.36)                        # 0 in the middle, 1 at the rim: lighter and yellower outward and upward
        up = 0.5 - 0.5 * math.sin(a)
        t = np.clip(0.25 + 0.45 * k + 0.3 * (up - 0.5) + rng.uniform(-0.12, 0.12), 0, 1)
        col = (int(255 * (0.13 + 0.30 * t)), int(255 * (0.30 + 0.42 * t)), int(255 * (0.08 + 0.10 * t)), 255)
        vein = tuple(int(c * 0.72) for c in col[:3]) + (255,)
        ln = n * rng.uniform(0.15, 0.22)
        leaf(d, x, y, ln, ln * rng.uniform(0.42, 0.56), a + rng.uniform(-0.9, 0.9), col, vein)
    finish(img, "leaf_cluster.png")


def pine_bough():
    rng = np.random.default_rng(202)
    n = S * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    mid = n / 2
    d.line([(mid, n), (mid, n * 0.04)], fill=(70, 48, 28, 255), width=int(n * 0.016))
    for i in range(70):
        t = i / 69                                    # 0 at the base (bottom of the texture), 1 at the tip
        y = n * (1.0 - 0.02 - 0.94 * t)
        reach = n * (0.46 * math.sin(math.pi * min(1.0, 0.18 + 0.82 * (1 - t) ** 0.7)) + 0.03)
        for side in (-1, 1):
            ang = rng.uniform(0.25, 0.75)             # the needle sweeps forward towards the tip
            x2 = mid + side * reach * rng.uniform(0.75, 1.0) * math.cos(ang)
            y2 = y - reach * 0.55 * math.sin(ang) * rng.uniform(0.6, 1.0)
            g = rng.uniform(0.0, 1.0)
            col = (int(255 * (0.08 + 0.10 * g + 0.10 * t)), int(255 * (0.24 + 0.22 * g + 0.10 * t)), int(255 * (0.10 + 0.05 * g)), 255)
            d.line([(mid, y), (x2, y2)], fill=col, width=int(n * 0.014))
    finish(img, "pine_bough.png")


def bark():
    rng = np.random.default_rng(303)
    size = S
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    warp = (fractal(rng, 1.6, size) - 0.5) * 26                 # the ridges wander sideways down the trunk
    xs = (x + warp) / size * 2 * np.pi
    ridges = np.zeros((size, size), np.float32)
    for k in range(3, 22):                                       # ridge frequencies (whole cycles, so the tile repeats)
        ridges += rng.uniform(0.3, 1.0) / math.sqrt(k) * np.sin(k * xs + rng.uniform(0, 6.28))
    ridges = (ridges - ridges.min()) / np.ptp(ridges)
    patch = fractal(rng, 1.2, size)
    deep = ridges < 0.22                                         # the furrows between plates
    v = 0.34 + 0.34 * ridges + 0.12 * (patch - 0.5) + 0.05 * (fractal(rng, 0.3, size) - 0.5)
    v = np.where(deep, v * 0.6, v)
    color = np.stack([v * 1.00, v * 0.76, v * 0.55], -1)
    normal = normal_map(ridges * 1.0 + 0.25 * fractal(rng, 0.6, size), 4.5)
    Image.fromarray((np.clip(color, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, "bark_color.png"))
    Image.fromarray((np.clip(normal, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, "bark_normal.png"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for make in (leaf_cluster, pine_bough, bark):
        make()
        print("wrote", make.__name__)
