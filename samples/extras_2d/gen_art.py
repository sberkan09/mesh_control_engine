#!/usr/bin/env python3
# python3 gen_art.py assets/textures   (needs Pillow)
# Generates the extras_2d sample art (repository content): parallax backdrops, a lit brick
# tile with its normal map, an ember sprite and a warm color-grade LUT.
import math, random, sys
from PIL import Image
out = sys.argv[1]
random.seed(7)

def save(img, name): img.save(f"{out}/{name}")

# Sky: vertical gradient, dusk.
W, H = 64, 128
sky = Image.new("RGBA", (W, H))
for y in range(H):
    t = y / (H - 1)
    top, bot = (24, 22, 58), (214, 120, 84)
    c = tuple(int(top[i] + (bot[i] - top[i]) * t ** 1.4) for i in range(3))
    for x in range(W): sky.putpixel((x, y), c + (255,))
save(sky, "sky.png")

def ridge(w, h, base, amp, freqs, color, seed):
    rnd = random.Random(seed)
    phases = [rnd.random() * 6.28 for _ in freqs]
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for x in range(w):
        u = x / w * 2 * math.pi
        y = base + sum(a * math.sin(f * u + p) for (f, a), p in zip(freqs, phases)) * amp
        top = int(h - y)
        for yy in range(max(top, 0), h):
            shade = 1.0 - 0.25 * (yy - top) / max(h - top, 1)
            img.putpixel((x, yy), tuple(int(c * shade) for c in color) + (255,))
    return img
save(ridge(256, 96, 50, 18, [(1, 1.0), (3, 0.5), (7, 0.2)], (70, 52, 92), 1), "mountains.png")
save(ridge(256, 64, 30, 10, [(2, 1.0), (5, 0.4), (11, 0.15)], (46, 40, 60), 2), "hills.png")

# Brick tile (32×32) + normal map from its height field.
N = 32
height = [[0.0] * N for _ in range(N)]
color = Image.new("RGBA", (N, N))
for y in range(N):
    for x in range(N):
        row = y // 8
        off = 8 if row % 2 else 0
        mortar = (y % 8 == 0) or ((x + off) % 16 == 0)
        h = 0.0 if mortar else 1.0 - 0.15 * random.random()
        height[y][x] = h
        base = (90, 84, 80) if mortar else (150 + random.randint(-12, 12), 72 + random.randint(-8, 8), 52)
        color.putpixel((x, y), base + (255,))
save(color, "brick.png")
nrm = Image.new("RGBA", (N, N))
for y in range(N):
    for x in range(N):
        hl = height[y][(x - 1) % N]; hr = height[y][(x + 1) % N]
        hu = height[(y - 1) % N][x]; hd = height[(y + 1) % N][x]
        dx = (hr - hl) * 1.5
        dy = (hu - hd) * 1.5  # image rows go down; +Y in the normal map is up
        n = (-dx, -dy, 1.0)
        l = math.sqrt(sum(c * c for c in n))
        n = [c / l for c in n]
        nrm.putpixel((x, y), tuple(int((c * 0.5 + 0.5) * 255) for c in n) + (255,))
save(nrm, "brick_n.png")

# Ember: soft round dot.
E = 16
ember = Image.new("RGBA", (E, E))
for y in range(E):
    for x in range(E):
        d = math.hypot(x + 0.5 - E / 2, y + 0.5 - E / 2) / (E / 2)
        a = max(0.0, 1.0 - d) ** 1.5
        ember.putpixel((x, y), (255, 255, 255, int(a * 255)))
save(ember, "ember.png")

# Warm grade LUT: identity strip pushed toward amber with lifted shadows.
lut = Image.new("RGBA", (256, 16))
for y in range(16):
    for x in range(256):
        r, g, b = (x % 16) / 15, y / 15, (x // 16) / 15
        r2 = min(1.0, r * 1.06 + 0.03)
        g2 = min(1.0, g * 0.98 + 0.02)
        b2 = b * 0.85 + 0.04
        lut.putpixel((x, y), (int(r2 * 255), int(g2 * 255), int(b2 * 255), 255))
save(lut, "grade_warm.png")
