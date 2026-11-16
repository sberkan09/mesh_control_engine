#!/usr/bin/env python3
"""Writes the tiling ground textures of 09_openworld (asphalt, paving, grass, concrete) into assets/textures/.

    python3 samples/09_openworld/tools/gen_textures.py          (needs numpy and Pillow)

The textures are generated here, not downloaded: a colour map and a tangent-space normal map per surface, 512 x 512,
tileable (all noise is made in the Fourier domain and the normals are wrapped finite differences), seeded, so every run
writes the same files. The sidecars are written by gen_assets.py. The look is simple on purpose: tinted noise, joints
between paving slabs, blade streaks in the grass. The files are original to this repo (CC0).
"""
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "assets", "textures"))
N = 512


def fractal(rng, exponent, size=N):
    """Tileable noise in [0, 1]: white noise shaped to a 1/f^exponent spectrum."""
    f = np.fft.fftfreq(size)
    r = np.hypot(*np.meshgrid(f, f))
    r[0, 0] = 1.0
    spec = np.fft.fft2(rng.standard_normal((size, size))) / r ** exponent
    spec[0, 0] = 0
    v = np.real(np.fft.ifft2(spec))
    v -= v.min()
    return v / v.max()


def normal_map(height, strength):
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5 * strength
    dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5 * strength
    n = np.stack([-dx, dy, np.ones_like(height)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def save(name, color, normal):
    os.makedirs(OUT, exist_ok=True)
    c = (np.clip(color, 0, 1) * 255 + 0.5).astype(np.uint8)
    n = (np.clip(normal, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(c).save(os.path.join(OUT, name + "_color.jpg"), quality=90, subsampling=0)
    Image.fromarray(n).save(os.path.join(OUT, name + "_normal.jpg"), quality=92, subsampling=0)


def asphalt():
    rng = np.random.default_rng(31)
    coarse, mid, fine = fractal(rng, 1.6), fractal(rng, 1.1), fractal(rng, 0.2)
    grains = (fine > 0.93) * 0.18 - (fine < 0.05) * 0.06      # lighter stones, a few dark pits
    v = 0.17 + 0.07 * (coarse - 0.5) + 0.05 * (mid - 0.5) + grains
    cracks = np.abs(fractal(rng, 2.2) - 0.5) < 0.0025
    v = np.where(cracks, v * 0.78, v)
    color = np.stack([v * 1.00, v * 1.01, v * 1.05], -1)
    return color, normal_map(0.6 * mid + 0.4 * fine - 0.5 * cracks, 2.5)


def paving():
    rng = np.random.default_rng(70)
    slabs = 4
    cell = N // slabs
    tone = rng.uniform(-0.05, 0.05, (slabs, slabs))
    y, x = np.mgrid[0:N, 0:N]
    tile = tone[y // cell, x // cell]
    gx, gy = x % cell, y % cell
    joint = np.minimum.reduce([gx, gy, cell - 1 - gx, cell - 1 - gy]) < 3
    grit = fractal(rng, 1.0)
    v = 0.55 + tile + 0.10 * (grit - 0.5) + 0.04 * (fractal(rng, 0.2) - 0.5)
    v = np.where(joint, 0.28 + 0.1 * grit, v)
    color = np.stack([v * 1.00, v * 0.98, v * 0.94], -1)
    height = np.where(joint, 0.0, 0.8) + 0.2 * grit
    return color, normal_map(height, 3.5)


def grass():
    rng = np.random.default_rng(4)
    patch, blades = fractal(rng, 1.8), fractal(rng, 0.5)
    streak = np.roll(blades, 1, 0) * 0.5 + blades * 0.5
    g = 0.22 + 0.14 * (patch - 0.5) + 0.12 * (streak - 0.5)
    color = np.stack([0.60 * g + 0.04, g + 0.07, 0.28 * g + 0.01], -1)
    return color, normal_map(0.5 * streak + 0.5 * patch, 1.6)


def concrete():
    rng = np.random.default_rng(34)
    mid, fine = fractal(rng, 1.4), fractal(rng, 0.3)
    v = 0.60 + 0.10 * (mid - 0.5) + 0.06 * (fine - 0.5)
    color = np.stack([v, v * 0.99, v * 0.96], -1)
    return color, normal_map(0.7 * mid + 0.3 * fine, 1.2)


if __name__ == "__main__":
    for name, make in (("asphalt", asphalt), ("paving", paving), ("grass", grass), ("concrete", concrete)):
        save(name, *make())
        print("wrote", name)
