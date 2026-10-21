#!/usr/bin/env python3
"""Undelivered's art and sound: everything under assets/textures and assets/audio is generated here
(Pillow + NumPy -> PNG, NumPy -> WAV). Deterministic, so a re-run is a no-op diff.

    python3 projects/Undelivered/tools/gen_assets.py

The look is paper and lamplight (docs/design/undelivered.md §4): every background is a stack of flat
paper cut-outs with slightly ragged edges, all sharing one paper-grain normal map so the 2D lights
(Light2D) give them the time of day. Characters are paper dolls: body, head, hair, and a face of a
few strokes that changes with the expression.

The .mceasset sidecars are NOT written here: the MCP server's cook_run {"adopt": true} registers
new files (tools/author_scene_mcp.py does that and patches "srgb": false onto the normal map and
the LUTs), which is the path a person's editor takes.
"""

import math
import pathlib
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = pathlib.Path(__file__).resolve().parents[1]
TEX = ROOT / "assets" / "textures"
AUD = ROOT / "assets" / "audio"
W, H = 1280, 720
SR = 48000

# ---- palette ------------------------------------------------------------------------------------------
PAPER = (236, 226, 204, 255)
INK = (52, 44, 40, 255)


def rgb(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


# ---- drawing helpers ----------------------------------------------------------------------------------

class Rng:
    def __init__(self, seed):
        self.r = np.random.default_rng(seed)

    def u(self, a=0.0, b=1.0):
        return float(self.r.uniform(a, b))


def ragged(points, rng, amount=3.0, step=18):
    """Subdivide a polygon's edges and jitter the new points: a torn-paper edge."""
    out = []
    n = len(points)
    for i in range(n):
        x0, y0 = points[i]
        x1, y1 = points[(i + 1) % n]
        d = math.hypot(x1 - x0, y1 - y0)
        k = max(1, int(d / step))
        for j in range(k):
            t = j / k
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            if j:
                x += rng.u(-amount, amount)
                y += rng.u(-amount, amount)
            out.append((x, y))
    return out


def layer():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))


def poly(img, points, color, rng=None, amount=3.0, shadow=True):
    d = ImageDraw.Draw(img)
    pts = ragged(points, rng, amount) if rng else points
    if shadow:
        sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
        ImageDraw.Draw(sh).polygon([(x + 4, y + 6) for x, y in pts], fill=(30, 20, 10, 70))
        sh = sh.filter(ImageFilter.GaussianBlur(5))
        img.alpha_composite(sh)
    d.polygon(pts, fill=color)
    return img


def rect(img, x0, y0, x1, y1, color, rng=None, amount=3.0, shadow=True):
    return poly(img, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], color, rng, amount, shadow)


def ellipse(img, cx, cy, rx, ry, color, rng=None, amount=2.0, shadow=True, n=40):
    pts = [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]
    return poly(img, pts, color, rng, amount, shadow)


def band(img, y0, y1, color):
    ImageDraw.Draw(img).rectangle([0, y0, W, y1], fill=color)


def grain(img, rng, strength=10):
    """Paper grain over the opaque pixels."""
    a = np.asarray(img).astype(np.int16)
    noise = rng.r.integers(-strength, strength + 1, size=(H, W, 1), dtype=np.int16)
    a[:, :, :3] = np.clip(a[:, :, :3] + noise * (a[:, :, 3:4] > 0), 0, 255)
    return Image.fromarray(a.astype(np.uint8), "RGBA")


def save(img, name):
    path = TEX / f"{name}.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path, optimize=True)


# ---- the paper normal map and the LUTs ------------------------------------------------------------------

def paper_normal(size=512, seed=3):
    rng = np.random.default_rng(seed)
    h = np.zeros((size, size), dtype=np.float32)
    for octave, amp in ((4, 1.0), (16, 0.5), (64, 0.25), (size, 0.12)):
        small = rng.random((octave, octave)).astype(np.float32)
        img = Image.fromarray((small * 255).astype(np.uint8)).resize((size, size), Image.BICUBIC)
        h += np.asarray(img).astype(np.float32) / 255.0 * amp
    # fibres: thin horizontal streaks
    fib = rng.random((size, size)).astype(np.float32)
    fib = np.asarray(Image.fromarray((fib * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6))) / 255.0
    h += (fib - 0.5) * 0.25
    h /= h.max()
    dx = np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)
    dy = np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)
    strength = 2.2
    n = np.stack([-dx * strength, dy * strength, np.ones_like(h)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    rgba = np.zeros((size, size, 4), dtype=np.uint8)
    rgba[:, :, :3] = ((n * 0.5 + 0.5) * 255).astype(np.uint8)
    rgba[:, :, 3] = 255
    return Image.fromarray(rgba, "RGBA")


def lut(kind):
    """256x16 strip LUT (16 slices of 16x16), imported with srgb off."""
    img = np.zeros((16, 256, 4), dtype=np.float32)
    for b in range(16):
        for g in range(16):
            for r in range(16):
                rr, gg, bb = r / 15.0, g / 15.0, b / 15.0
                lum = 0.3 * rr + 0.59 * gg + 0.11 * bb
                if kind == "warm":
                    rr, gg, bb = min(1, rr * 1.04 + 0.02), gg, bb * 0.92
                elif kind == "cold":
                    rr, gg, bb = rr * 0.9, gg * 0.97, min(1, bb * 1.06 + 0.03)
                elif kind == "sepia":
                    rr, gg, bb = min(1, lum * 1.05 + 0.08), lum * 0.9 + 0.04, lum * 0.68
                elif kind == "night":
                    rr, gg, bb = rr * 0.75, gg * 0.82, min(1, bb * 1.05 + 0.04)
                # a touch of contrast for every grade
                def c(v):
                    return min(1.0, max(0.0, (v - 0.5) * 1.08 + 0.5))
                img[g, b * 16 + r] = (c(rr) * 255, c(gg) * 255, c(bb) * 255, 255)
    return Image.fromarray(img.astype(np.uint8), "RGBA")


# ---- locations (far -> near layers; each a full-frame RGBA) ---------------------------------------------

def sky(color_top, color_bottom):
    img = layer()
    a = np.zeros((H, W, 4), dtype=np.float32)
    t = np.linspace(0, 1, H)[:, None]
    for c in range(3):
        a[:, :, c] = color_top[c] * (1 - t) + color_bottom[c] * t
    a[:, :, 3] = 255
    return Image.fromarray(a.astype(np.uint8), "RGBA")


def sea(img, rng, y0, color, foam):
    band(img, y0, H, color)
    for k in range(14):
        y = y0 + 12 + k * 22 + rng.u(-4, 4)
        x = rng.u(-200, 200)
        for s in range(6):
            ImageDraw.Draw(img).line([(x + s * 260, y), (x + s * 260 + 70 + rng.u(0, 80), y + rng.u(-2, 2))], fill=foam, width=2)
    return img


def loc_pier():
    rng = Rng(11)
    layers = []
    layers.append(("sky", sky(rgb("#d9c7a8"), rgb("#f1dfc0"))))
    far = layer()
    poly(far, [(0, 430), (170, 395), (330, 410), (470, 380), (640, 400), (760, 372), (980, 395), (1280, 380), (1280, 470), (0, 470)],
         rgb("#b7a68a"), rng, 2.5)  # the mainland, hazy
    layers.append(("far", far))
    water = layer()
    sea(water, rng, 445, rgb("#5f8a94"), rgb("#8fb3b7"))
    poly(water, [(0, 455), (420, 448), (430, 470), (0, 478)], rgb("#8d8470"), rng, 2)  # the breakwater
    layers.append(("sea", water))
    ferry = layer()
    poly(ferry, [(560, 500), (1060, 500), (1020, 560), (600, 560)], rgb("#3d4a5a"), rng, 2)
    rect(ferry, 620, 440, 990, 500, rgb("#efe6d2"), rng, 2)
    rect(ferry, 700, 395, 900, 440, rgb("#e2d7bf"), rng, 2)
    rect(ferry, 740, 350, 790, 395, rgb("#c7563f"), rng, 2)
    for k in range(9):
        rect(ferry, 640 + k * 38, 455, 664 + k * 38, 480, rgb("#f6d58a"), None, shadow=False)
    for k in range(4):
        rect(ferry, 720 + k * 42, 405, 744 + k * 42, 428, rgb("#f6d58a"), None, shadow=False)
    ImageDraw.Draw(ferry).text((665, 505), "KUZGUN II", fill=rgb("#efe6d2"))
    layers.append(("ferry", ferry))
    pier = layer()
    poly(pier, [(0, 580), (1280, 600), (1280, 720), (0, 720)], rgb("#7a6446"), rng, 3)
    for k in range(8):
        x = 40 + k * 170
        rect(pier, x, 540, x + 26, 640, rgb("#4e3d2b"), rng, 2)
    rect(pier, 1130, 470, 1146, 640, rgb("#3a3a3a"), rng, 1)  # the pier lamp
    ellipse(pier, 1138, 462, 26, 20, rgb("#f6d58a"), rng, 1)
    layers.append(("pier", pier))
    return layers


def loc_cafe():
    rng = Rng(12)
    layers = []
    through = sky(rgb("#cfd8dc"), rgb("#f0e4c8"))
    sea(through, rng, 380, rgb("#6b9aa3"), rgb("#9ec3c6"))
    layers.append(("window", through))
    room = layer()
    rect(room, 0, 0, W, H, rgb("#d8c7a4"), None, shadow=False)
    # cut the window out
    cut = Image.new("L", (W, H), 255)
    ImageDraw.Draw(cut).rectangle([760, 90, 1180, 420], fill=0)
    room.putalpha(cut)
    d = ImageDraw.Draw(room)
    for k in range(6):
        d.rectangle([0, 500 + k * 36, W, 502 + k * 36], fill=rgb("#c3b08a"))  # floorboards
    rect(room, 740, 70, 1200, 92, rgb("#8d6b45"), rng, 2)   # window frame
    rect(room, 740, 420, 1200, 440, rgb("#8d6b45"), rng, 2)
    rect(room, 960, 90, 980, 420, rgb("#8d6b45"), rng, 1)
    rect(room, 60, 120, 420, 150, rgb("#8d6b45"), rng, 2)   # a shelf
    for k, c in enumerate(("#9f3b2f", "#2f6f6b", "#c89a3c", "#5a4b8a", "#9f3b2f")):
        rect(room, 90 + k * 60, 60, 128 + k * 60, 120, rgb(c), rng, 2)
    layers.append(("room", room))
    counter = layer()
    poly(counter, [(0, 460), (560, 460), (580, 720), (0, 720)], rgb("#6b4e32"), rng, 3)
    rect(counter, 0, 440, 575, 470, rgb("#8d6b45"), rng, 2)
    rect(counter, 60, 360, 240, 440, rgb("#4b3a2a"), rng, 2)  # the radio
    ellipse(counter, 110, 400, 28, 28, rgb("#e0cf9c"), rng, 1)
    rect(counter, 160, 380, 220, 420, rgb("#f0b84a"), None, shadow=False)  # the dial
    for k in range(5):
        rect(counter, 300 + k * 44, 400, 328 + k * 44, 440, rgb("#e9dcc0"), rng, 1)  # glasses
    ellipse(counter, 430, 380, 50, 40, rgb("#b3452f"), rng, 1)  # the samovar
    rect(counter, 420, 320, 440, 380, rgb("#b3452f"), rng, 1)
    layers.append(("counter", counter))
    tables = layer()
    ellipse(tables, 1050, 640, 230, 60, rgb("#8d6b45"), rng, 2)
    rect(tables, 1030, 640, 1070, 720, rgb("#5a4530"), rng, 1)
    ellipse(tables, 1010, 600, 20, 26, rgb("#e9dcc0"), rng, 1)  # a tea glass
    ellipse(tables, 1120, 606, 20, 26, rgb("#e9dcc0"), rng, 1)
    layers.append(("tables", tables))
    return layers


def loc_postoffice():
    rng = Rng(13)
    layers = []
    wall = layer()
    rect(wall, 0, 0, W, H, rgb("#c9b89a"), None, shadow=False)
    for row in range(4):
        y = 90 + row * 95
        rect(wall, 60, y, 700, y + 14, rgb("#6b4e32"), rng, 2)
        for k in range(9):
            if (k + row) % 3:
                rect(wall, 80 + k * 68, y - 60, 130 + k * 68, y, rgb(("#e4d7bb", "#d6c4a0", "#bfae8c")[(k + row) % 3]), rng, 1)
    rect(wall, 880, 100, 1180, 400, rgb("#8ea7b3"), rng, 2)  # the window, night sea
    rect(wall, 860, 80, 1200, 100, rgb("#6b4e32"), rng, 1)
    rect(wall, 860, 400, 1200, 420, rgb("#6b4e32"), rng, 1)
    rect(wall, 1020, 100, 1040, 400, rgb("#6b4e32"), rng, 1)
    layers.append(("wall", wall))
    counter = layer()
    poly(counter, [(0, 470), (1280, 470), (1280, 720), (0, 720)], rgb("#5a4530"), rng, 3)
    rect(counter, 0, 450, W, 480, rgb("#8d6b45"), rng, 2)
    rect(counter, 420, 510, 820, 600, rgb("#4b3a2a"), rng, 2)   # the drawer
    ellipse(counter, 620, 555, 14, 10, rgb("#c9a04a"), rng, 1)  # its brass pull
    rect(counter, 80, 300, 100, 450, rgb("#3a3a3a"), rng, 1)    # the counter lamp
    poly(counter, [(40, 300), (140, 300), (120, 250), (60, 250)], rgb("#2f5d4f"), rng, 1)
    ellipse(counter, 90, 300, 40, 14, rgb("#f6d58a"), None, shadow=False)
    for k in range(3):
        rect(counter, 950 + k * 90, 420, 1020 + k * 90, 450, rgb("#efe6d2"), rng, 1)  # stacked post
    layers.append(("counter", counter))
    return layers


def loc_lighthouse():
    rng = Rng(14)
    layers = []
    layers.append(("sky", sky(rgb("#2a3550"), rgb("#8a6a6e"))))
    water = layer()
    sea(water, rng, 440, rgb("#2f4a5a"), rgb("#5a7a88"))
    layers.append(("sea", water))
    lamp = layer()
    rect(lamp, 300, 60, 980, 520, rgb("#5a5048"), rng, 3)   # the lamp room
    rect(lamp, 330, 90, 950, 500, rgb("#1c1a1a"), rng, 2)
    for k in range(6):
        rect(lamp, 330 + k * 110, 90, 346 + k * 110, 500, rgb("#6b6058"), rng, 1)
    ellipse(lamp, 640, 300, 110, 130, rgb("#f6d58a"), rng, 2)  # the lens
    ellipse(lamp, 640, 300, 70, 90, rgb("#fff1c0"), None, shadow=False)
    layers.append(("lamp", lamp))
    rail = layer()
    poly(rail, [(0, 560), (1280, 560), (1280, 720), (0, 720)], rgb("#4a4440"), rng, 3)
    rect(rail, 0, 470, W, 486, rgb("#2b2826"), rng, 1)
    for k in range(13):
        rect(rail, 20 + k * 102, 486, 32 + k * 102, 560, rgb("#2b2826"), rng, 1)
    layers.append(("rail", rail))
    return layers


def loc_radio():
    rng = Rng(15)
    layers = []
    wall = layer()
    rect(wall, 0, 0, W, H, rgb("#5c5a52"), None, shadow=False)
    rect(wall, 820, 110, 1140, 380, rgb("#1d2a40"), rng, 2)   # the night window
    for k in range(25):
        ImageDraw.Draw(wall).point((830 + rng.u(0, 300), 120 + rng.u(0, 200)), fill=rgb("#f3e9c8"))
    rect(wall, 800, 90, 1160, 110, rgb("#3b352e"), rng, 1)
    rect(wall, 800, 380, 1160, 400, rgb("#3b352e"), rng, 1)
    layers.append(("wall", wall))
    desk = layer()
    poly(desk, [(0, 500), (1280, 500), (1280, 720), (0, 720)], rgb("#6b4e32"), rng, 3)
    rect(desk, 180, 300, 720, 500, rgb("#3d3a36"), rng, 2)    # the set
    rect(desk, 220, 340, 460, 400, rgb("#131313"), rng, 1)
    ellipse(desk, 340, 370, 90, 22, rgb("#f0b84a"), None, shadow=False)  # the dial
    for k in range(5):
        ellipse(desk, 520 + k * 38, 440, 12, 12, rgb("#9a8a6a"), rng, 1)
    rect(desk, 760, 380, 790, 500, rgb("#c9c0a8"), rng, 1)    # a candle
    ellipse(desk, 775, 370, 10, 18, rgb("#ffd27a"), None, shadow=False)
    rect(desk, 900, 440, 1180, 500, rgb("#efe6d2"), rng, 1)   # papers
    layers.append(("desk", desk))
    return layers


def loc_cove():
    rng = Rng(16)
    layers = []
    layers.append(("sky", sky(rgb("#f0cfa0"), rgb("#f7e6c6"))))
    water = layer()
    sea(water, rng, 400, rgb("#5f9aa3"), rgb("#a4cfd0"))
    layers.append(("sea", water))
    beach = layer()
    poly(beach, [(0, 520), (300, 505), (700, 530), (1000, 510), (1280, 525), (1280, 720), (0, 720)], rgb("#d9c39a"), rng, 3)
    for k in range(30):
        ellipse(beach, rng.u(0, W), rng.u(560, 700), rng.u(4, 12), rng.u(3, 8), rgb("#b5a082"), None, 0, False)
    layers.append(("beach", beach))
    boats = layer()
    poly(boats, [(140, 560), (560, 560), (520, 630), (190, 630)], rgb("#2f5f8a"), rng, 2)   # the working boat
    rect(boats, 150, 530, 560, 562, rgb("#e9dcc0"), rng, 2)
    poly(boats, [(720, 575), (1120, 575), (1090, 640), (760, 640)], rgb("#7a6446"), rng, 2)  # the broken one
    rect(boats, 740, 545, 1110, 577, rgb("#a28a65"), rng, 2)
    rect(boats, 900, 520, 980, 548, rgb("#5a4530"), rng, 1)
    layers.append(("boats", boats))
    nets = layer()
    for k in range(3):
        x = 60 + k * 90
        rect(nets, x, 440, x + 10, 700, rgb("#5a4530"), rng, 1)
    d = ImageDraw.Draw(nets)
    for k in range(12):
        d.line([(70, 470 + k * 18), (330, 490 + k * 18)], fill=rgb("#8d7a5a"), width=2)
    for k in range(14):
        d.line([(70 + k * 20, 470), (90 + k * 20, 700)], fill=rgb("#8d7a5a"), width=1)
    layers.append(("nets", nets))
    return layers


def loc_school():
    rng = Rng(17)
    layers = []
    layers.append(("sky", sky(rgb("#bcd0dc"), rgb("#f2e8d0"))))
    building = layer()
    rect(building, 160, 150, 1120, 520, rgb("#e0cfae"), rng, 3)
    poly(building, [(130, 150), (1150, 150), (1080, 80), (200, 80)], rgb("#a8503c"), rng, 2)  # the tiled roof
    for k in range(4):
        rect(building, 240 + k * 230, 220, 380 + k * 230, 400, rgb("#3f4a52"), rng, 2)      # shut windows
        for j in range(4):
            rect(building, 240 + k * 230, 220 + j * 46, 380 + k * 230, 232 + j * 46, rgb("#6b6058"), None, shadow=False)
    rect(building, 600, 400, 680, 520, rgb("#6b4e32"), rng, 2)  # the door
    layers.append(("building", building))
    yard = layer()
    poly(yard, [(0, 520), (1280, 520), (1280, 720), (0, 720)], rgb("#c9b08a"), rng, 3)
    rect(yard, 1180, 120, 1194, 560, rgb("#8e8e8e"), rng, 1)  # the flagpole, no flag
    for k in range(6):
        ImageDraw.Draw(yard).line([(0, 560 + k * 28), (W, 566 + k * 28)], fill=rgb("#b89f78"), width=2)
    layers.append(("yard", yard))
    return layers


def loc_grocery():
    rng = Rng(18)
    layers = []
    street = layer()
    rect(street, 0, 0, W, H, rgb("#e6dcc6"), None, shadow=False)
    rect(street, 900, 80, 1240, 500, rgb("#9cc0cc"), rng, 2)  # the open door: the street beyond
    poly(street, [(900, 380), (1240, 380), (1240, 500), (900, 500)], rgb("#c9b08a"), rng, 2)
    rect(street, 880, 60, 1260, 80, rgb("#6b4e32"), rng, 1)
    rect(street, 870, 60, 900, 500, rgb("#6b4e32"), rng, 1)
    rect(street, 1240, 60, 1270, 500, rgb("#6b4e32"), rng, 1)
    layers.append(("street", street))
    shelves = layer()
    for row in range(4):
        y = 120 + row * 100
        rect(shelves, 40, y + 60, 800, y + 74, rgb("#6b4e32"), rng, 2)
        for k in range(11):
            c = ("#c9a04a", "#9f3b2f", "#2f6f6b", "#e9dcc0", "#5a4b8a")[(k * 7 + row) % 5]
            rect(shelves, 60 + k * 66, y + 10, 104 + k * 66, y + 60, rgb(c), rng, 1)
    layers.append(("shelves", shelves))
    counter = layer()
    poly(counter, [(0, 520), (1280, 520), (1280, 720), (0, 720)], rgb("#8d6b45"), rng, 3)
    rect(counter, 0, 500, W, 530, rgb("#a28a65"), rng, 2)
    rect(counter, 300, 360, 520, 500, rgb("#4b3a2a"), rng, 2)   # the scale
    ellipse(counter, 410, 400, 60, 60, rgb("#e9dcc0"), rng, 1)
    rect(counter, 260, 340, 560, 362, rgb("#c9a04a"), rng, 1)
    ellipse(counter, 700, 470, 60, 24, rgb("#c9a04a"), rng, 1)  # a sack
    rect(counter, 1000, 300, 1020, 500, rgb("#3a3a3a"), rng, 1)  # the bulb's cord
    ellipse(counter, 1010, 300, 18, 24, rgb("#f6d58a"), None, shadow=False)
    layers.append(("counter", counter))
    return layers


def loc_drawer():
    """The drawer close-up: the open drawer seen from above, seven slots for the envelope sprites."""
    rng = Rng(19)
    layers = []
    top = layer()
    rect(top, 0, 0, W, H, rgb("#5a4530"), None, shadow=False)
    rect(top, 120, 80, 1160, 660, rgb("#3a2d20"), rng, 3)
    rect(top, 150, 110, 1130, 630, rgb("#6b5a44"), rng, 2)
    for k in range(5):
        ImageDraw.Draw(top).line([(150, 140 + k * 110), (1130, 142 + k * 110)], fill=rgb("#4d3f2e"), width=2)
    layers.append(("drawer", top))
    return layers


LOCATIONS = {
    "pier": loc_pier, "cafe": loc_cafe, "postoffice": loc_postoffice, "lighthouse": loc_lighthouse,
    "radio": loc_radio, "cove": loc_cove, "school": loc_school, "grocery": loc_grocery, "drawer": loc_drawer,
}


# ---- characters: paper dolls ----------------------------------------------------------------------------

CAST = {
    # name: (skin, hair, hair style, body colour, trim colour, build)
    "mira": ("#e8c4a0", "#2b1d16", "long", "#c7563f", "#efe6d2", 0.92),
    "feride": ("#dcb690", "#4a3a3a", "bun", "#2f6f6b", "#e9dcc0", 1.08),
    "selin": ("#e3bf9c", "#6b4a2a", "short", "#5a7a88", "#efe6d2", 0.95),
    "halil": ("#c9a07e", "#b9b4ab", "cap", "#3d4a5a", "#8d8470", 1.1),
    "yusuf": ("#b98a66", "#d8d2c6", "bald", "#6b4e32", "#c9a04a", 1.15),
    "hasan": ("#d2a784", "#8a8078", "cap", "#1f2f44", "#f6d58a", 1.12),
    "cemil": ("#dcb690", "#3a2a22", "side", "#8a7a5a", "#efe6d2", 1.2),
}
EXPRESSIONS = ("neutral", "warm", "troubled", "sharp", "open")
POSES = ("stand", "gesture")


def doll(name, expression, pose):
    skin, hair, style, body, trim, build = CAST[name]
    cw, ch = 360, 720
    img = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    rng = Rng(hash(name) & 0xFFFF)
    cx = cw / 2
    bw = 150 * build
    # body
    poly(img, [(cx - bw * 0.8, 300), (cx + bw * 0.8, 300), (cx + bw, 720), (cx - bw, 720)], rgb(body), rng, 2.5)
    # collar / trim
    poly(img, [(cx - 40, 300), (cx + 40, 300), (cx + 24, 360), (cx - 24, 360)], rgb(trim), rng, 1.5)
    # arms
    if pose == "stand":
        poly(img, [(cx - bw * 0.8, 320), (cx - bw * 0.55, 320), (cx - bw * 0.7, 600), (cx - bw * 1.05, 600)], rgb(body), rng, 2)
        poly(img, [(cx + bw * 0.8, 320), (cx + bw * 0.55, 320), (cx + bw * 0.7, 600), (cx + bw * 1.05, 600)], rgb(body), rng, 2)
    else:
        poly(img, [(cx - bw * 0.8, 320), (cx - bw * 0.55, 320), (cx - bw * 0.7, 600), (cx - bw * 1.05, 600)], rgb(body), rng, 2)
        poly(img, [(cx + bw * 0.8, 320), (cx + bw * 0.55, 330), (cx + bw * 1.4, 430), (cx + bw * 1.5, 400)], rgb(body), rng, 2)
        ellipse(img, cx + bw * 1.48, 400, 22, 26, rgb(skin), rng, 1)
    # neck and head
    rect(img, cx - 26, 250, cx + 26, 310, rgb(skin), rng, 1.5)
    ellipse(img, cx, 190, 78, 92, rgb(skin), rng, 2)
    # hair
    if style == "long":
        poly(img, [(cx - 90, 170), (cx + 90, 170), (cx + 100, 380), (cx - 100, 380)], rgb(hair), rng, 2, shadow=False)
        ellipse(img, cx, 150, 86, 70, rgb(hair), rng, 2, shadow=False)
        ellipse(img, cx, 200, 66, 80, rgb(skin), None, 0, False)  # the face over the hair
    elif style == "bun":
        ellipse(img, cx, 140, 84, 60, rgb(hair), rng, 2, shadow=False)
        ellipse(img, cx + 60, 110, 34, 34, rgb(hair), rng, 2, shadow=False)
    elif style == "short":
        ellipse(img, cx, 140, 84, 58, rgb(hair), rng, 2, shadow=False)
        poly(img, [(cx - 84, 150), (cx - 60, 230), (cx - 86, 240)], rgb(hair), rng, 1, shadow=False)
    elif style == "cap":
        ellipse(img, cx, 125, 86, 44, rgb(hair), rng, 2, shadow=False)
        poly(img, [(cx - 92, 128), (cx + 92, 128), (cx + 100, 142), (cx - 100, 142)], rgb(hair), rng, 1, shadow=False)
    elif style == "bald":
        ellipse(img, cx - 70, 190, 18, 40, rgb(hair), rng, 1, shadow=False)
        ellipse(img, cx + 70, 190, 18, 40, rgb(hair), rng, 1, shadow=False)
        ellipse(img, cx, 260, 50, 30, rgb(hair), rng, 1, shadow=False)  # a moustache-beard
    elif style == "side":
        ellipse(img, cx, 135, 84, 50, rgb(hair), rng, 2, shadow=False)
        poly(img, [(cx - 84, 140), (cx + 30, 120), (cx + 84, 150), (cx - 84, 170)], rgb(hair), rng, 1, shadow=False)
    # face: a few strokes
    d = ImageDraw.Draw(img)
    ink = rgb("#3a2c24")
    ey = 190
    if expression == "warm":
        d.arc([cx - 46, ey - 8, cx - 18, ey + 10], 200, 340, fill=ink, width=4)
        d.arc([cx + 18, ey - 8, cx + 46, ey + 10], 200, 340, fill=ink, width=4)
        d.arc([cx - 30, 220, cx + 30, 256], 10, 170, fill=ink, width=4)
    elif expression == "troubled":
        d.ellipse([cx - 40, ey - 6, cx - 24, ey + 10], fill=ink)
        d.ellipse([cx + 24, ey - 6, cx + 40, ey + 10], fill=ink)
        d.line([(cx - 48, ey - 24), (cx - 20, ey - 16)], fill=ink, width=4)
        d.line([(cx + 20, ey - 16), (cx + 48, ey - 24)], fill=ink, width=4)
        d.arc([cx - 24, 232, cx + 24, 256], 190, 350, fill=ink, width=4)
    elif expression == "sharp":
        d.ellipse([cx - 40, ey - 4, cx - 24, ey + 8], fill=ink)
        d.ellipse([cx + 24, ey - 4, cx + 40, ey + 8], fill=ink)
        d.line([(cx - 48, ey - 18), (cx - 20, ey - 24)], fill=ink, width=4)
        d.line([(cx + 20, ey - 24), (cx + 48, ey - 18)], fill=ink, width=4)
        d.line([(cx - 22, 244), (cx + 22, 240)], fill=ink, width=4)
    elif expression == "open":
        d.ellipse([cx - 42, ey - 10, cx - 22, ey + 12], fill=ink)
        d.ellipse([cx + 22, ey - 10, cx + 42, ey + 12], fill=ink)
        d.line([(cx - 46, ey - 28), (cx - 20, ey - 30)], fill=ink, width=3)
        d.line([(cx + 20, ey - 30), (cx + 46, ey - 28)], fill=ink, width=3)
        d.ellipse([cx - 14, 232, cx + 14, 258], outline=ink, width=4)
    else:
        d.ellipse([cx - 40, ey - 6, cx - 24, ey + 10], fill=ink)
        d.ellipse([cx + 24, ey - 6, cx + 40, ey + 10], fill=ink)
        d.line([(cx - 20, 244), (cx + 20, 244)], fill=ink, width=4)
    d.line([(cx, 200), (cx - 6, 226), (cx + 4, 228)], fill=ink, width=3)  # the nose
    return img


# ---- UI strips --------------------------------------------------------------------------------------------

def torn_strip(w, h, color, rng, torn_top=True, amount=4.0):
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    pts = [(0, 14), (w, 14), (w, h), (0, h)] if torn_top else [(0, 0), (w, 0), (w, h), (0, h)]
    pts = ragged(pts, rng, amount, 14)
    ImageDraw.Draw(img).polygon(pts, fill=color)
    return img


def dialogue_box():
    rng = Rng(21)
    img = torn_strip(1280, 230, (238, 229, 208, 236), rng)
    return img


def name_tag():
    rng = Rng(22)
    img = torn_strip(320, 60, (60, 48, 40, 240), rng, torn_top=False, amount=2.5)
    return img


def choice_row():
    rng = Rng(23)
    img = torn_strip(900, 62, (238, 229, 208, 232), rng, torn_top=False, amount=2.5)
    return img


def panel():
    rng = Rng(24)
    img = torn_strip(1100, 620, (34, 30, 28, 236), rng, torn_top=False, amount=3.0)
    return img


def letter_card():
    rng = Rng(25)
    img = torn_strip(1000, 640, (243, 234, 210, 250), rng, torn_top=False, amount=3.0)
    d = ImageDraw.Draw(img)
    for k in range(13):
        d.line([(80, 110 + k * 40), (920, 110 + k * 40)], fill=(190, 176, 150, 120), width=1)
    # a coffee ring
    d.ellipse([760, 470, 880, 560], outline=(170, 140, 100, 90), width=5)
    return grain(img.resize((W, H)).crop((0, 0, 1000, 640)), rng, 6) if False else img


def envelope():
    rng = Rng(26)
    img = Image.new("RGBA", (320, 200), (0, 0, 0, 0))
    rect(img, 6, 6, 314, 194, (240, 232, 212, 255), rng, 2)
    d = ImageDraw.Draw(img)
    d.polygon([(6, 6), (314, 6), (160, 110)], fill=(222, 212, 190, 255))
    d.line([(6, 6), (160, 110), (314, 6)], fill=(170, 155, 130, 255), width=2)
    d.rectangle([236, 20, 290, 70], outline=(150, 60, 50, 255), width=2)
    return img


def stamp():
    img = Image.new("RGBA", (200, 90), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([4, 4, 196, 86], radius=8, outline=(150, 40, 40, 230), width=5)
    d.text((26, 30), "DELIVERED", fill=(150, 40, 40, 230))
    return img.rotate(-8, expand=False, resample=Image.BICUBIC)


def fade_quad():
    return Image.new("RGBA", (8, 8), (0, 0, 0, 255))


def glow():
    """A soft disc for lamps and the lighthouse beam (additive-looking through alpha)."""
    s = 128
    y, x = np.mgrid[0:s, 0:s]
    d = np.sqrt((x - s / 2) ** 2 + (y - s / 2) ** 2) / (s / 2)
    a = np.clip(1 - d, 0, 1) ** 2
    img = np.zeros((s, s, 4), dtype=np.uint8)
    img[:, :, :3] = (255, 230, 170)
    img[:, :, 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(img, "RGBA")


# ---- audio --------------------------------------------------------------------------------------------------

def write_wav(path, samples):
    data = np.clip(samples, -1, 1)
    pcm = (data * 32767).astype("<i2").tobytes()
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm)


def t_axis(seconds):
    return np.arange(int(SR * seconds), dtype=np.float32) / SR


def lowpass(x, cutoff):
    rc = 1.0 / (2 * np.pi * cutoff)
    a = (1.0 / SR) / (rc + 1.0 / SR)
    y = np.empty_like(x)
    acc = 0.0
    # a cheap one-pole, vectorised in blocks is awkward; the loop is fine for a few hundred k samples
    for i in range(0, len(x), 4096):
        blk = x[i:i + 4096]
        out = np.empty_like(blk)
        for j in range(len(blk)):
            acc += a * (blk[j] - acc)
            out[j] = acc
        y[i:i + 4096] = out
    return y


def lowpass_fast(x, cutoff):
    k = max(1, int(SR / cutoff / 2))
    kernel = np.ones(k, dtype=np.float32) / k
    return np.convolve(x, kernel, mode="same").astype(np.float32)


def seamless(s, seconds=0.4):
    n = int(SR * seconds)
    ramp = np.linspace(0, 1, n, dtype=np.float32)
    s = s.copy()
    s[-n:] = s[-n:] * (1 - ramp) + s[:n] * ramp
    return s


def pluck(freq, t, decay=3.0, bright=0.3):
    return (np.sin(2 * np.pi * freq * t) + bright * np.sin(2 * np.pi * freq * 2 * t) + bright * 0.4 * np.sin(2 * np.pi * freq * 3 * t)) * np.exp(-t * decay)


def pad(freqs, t, det=0.3, gain=0.06):
    s = np.zeros_like(t)
    for f in freqs:
        for d in (-det, 0.0, det):
            s += np.sin(2 * np.pi * f * (1 + d / 100.0) * t) * gain
    return s


NOTE = {n: 440.0 * 2 ** ((i - 9) / 12.0) for i, n in enumerate(["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"])}


def note(name, octave=4):
    return NOTE[name] * 2 ** (octave - 4)


def sequence(events, length, decay=3.0, bright=0.3, gain=0.25):
    """events: (time, freq) plucks over `length` seconds."""
    t = t_axis(length)
    s = np.zeros_like(t)
    for when, freq in events:
        i0 = int(when * SR)
        seg = t[: len(t) - i0]
        s[i0:] += pluck(freq, seg, decay, bright) * gain
    return s


def music_harbour():
    length = 24.0
    t = t_axis(length)
    chords = [[note("D", 3), note("F", 3), note("A", 3), note("C", 4)], [note("A#", 2), note("D", 3), note("F", 3), note("A", 3)],
              [note("G", 2), note("A#", 2), note("D", 3), note("F", 3)], [note("A", 2), note("C#", 3), note("E", 3), note("G", 3)]]
    s = np.zeros_like(t)
    for i, ch in enumerate(chords):
        seg = (t >= i * 6) & (t < (i + 1) * 6)
        tt = t[seg] - i * 6
        fade = np.minimum(np.minimum(tt / 1.2, 1.0), (6 - tt) / 1.2)
        s[seg] += pad(ch, tt, gain=0.05) * fade * (0.85 + 0.15 * np.sin(2 * np.pi * 0.2 * tt))
    arp = []
    for i, ch in enumerate(chords):
        for k in range(12):
            arp.append((i * 6 + k * 0.5, ch[k % 4] * 2))
    s += sequence(arp, length, decay=2.5, bright=0.2, gain=0.12)
    return seamless(lowpass_fast(s, 3000)) * 0.8


def music_cafe():
    length = 16.0
    chords = [[note("G", 3), note("B", 3), note("D", 4)], [note("C", 4), note("E", 4), note("G", 4)],
              [note("A", 3), note("C", 4), note("E", 4)], [note("D", 4), note("F#", 4), note("A", 4)]]
    ev = []
    for i, ch in enumerate(chords):
        for k in range(8):
            ev.append((i * 4 + k * 0.5, ch[(k * 2) % 3] * (2 if k % 4 == 2 else 1)))
        ev.append((i * 4, ch[0] / 2))
    s = sequence(ev, length, decay=2.2, bright=0.35, gain=0.22)
    return seamless(lowpass_fast(s, 4000)) * 0.9


def music_drawer():
    length = 20.0
    ev = [(0.0, note("E", 4)), (1.5, note("G", 4)), (3.0, note("B", 4)), (5.0, note("A", 4)), (7.0, note("E", 4)),
          (10.0, note("D", 4)), (11.5, note("F#", 4)), (13.0, note("B", 4)), (15.5, note("A", 4)), (17.0, note("G", 4))]
    s = sequence(ev, length, decay=0.9, bright=0.15, gain=0.3)
    t = t_axis(length)
    s += pad([note("E", 2), note("B", 2)], t, gain=0.02) * np.minimum(np.minimum(t / 2, 1), (length - t) / 2)
    return seamless(lowpass_fast(s, 2500)) * 0.9


def music_light():
    length = 24.0
    t = t_axis(length)
    s = pad([note("C#", 3), note("G#", 3), note("B", 3), note("F#", 4)], t, det=0.5, gain=0.05)
    s *= 0.7 + 0.3 * np.sin(2 * np.pi * (1.0 / 12.0) * t)
    s += np.sin(2 * np.pi * note("C#", 2) * t) * 0.08 * (0.5 + 0.5 * np.sin(2 * np.pi * (1.0 / 6.0) * t))
    return seamless(lowpass_fast(s, 1800)) * 0.9


def music_cove():
    length = 18.0
    chords = [[note("A", 3), note("C", 4), note("E", 4)], [note("D", 3), note("F", 3), note("A", 3)],
              [note("E", 3), note("G#", 3), note("B", 3)], [note("A", 3), note("C", 4), note("E", 4)]]
    ev = []
    beat = 0.75
    for bar in range(8):
        ch = chords[bar % 4]
        base = bar * 3 * beat
        ev.append((base, ch[0] / 2))
        ev.append((base + beat, ch[1]))
        ev.append((base + 2 * beat, ch[2]))
    s = sequence(ev, length, decay=2.0, bright=0.3, gain=0.24)
    return seamless(lowpass_fast(s, 3500)) * 0.9


def music_sunday():
    length = 24.0
    t = t_axis(length)
    chords = [[note("F", 3), note("A", 3), note("C", 4), note("E", 4)], [note("C", 3), note("E", 3), note("G", 3), note("B", 3)],
              [note("D", 3), note("F", 3), note("A", 3), note("C", 4)], [note("A#", 2), note("D", 3), note("F", 3), note("A", 3)]]
    s = np.zeros_like(t)
    for i, ch in enumerate(chords):
        seg = (t >= i * 6) & (t < (i + 1) * 6)
        tt = t[seg] - i * 6
        fade = np.minimum(np.minimum(tt / 1.0, 1.0), (6 - tt) / 1.0)
        s[seg] += pad(ch, tt, gain=0.05) * fade
    bells = [(i * 6 + k * 1.5, chords[i][(k + 1) % 4] * 4) for i in range(4) for k in range(4)]
    s += sequence(bells, length, decay=1.6, bright=0.1, gain=0.08)
    return seamless(lowpass_fast(s, 3200)) * 0.85


def amb_sea(rng):
    length = 20.0
    t = t_axis(length)
    n = rng.r.normal(0, 1, len(t)).astype(np.float32)
    n = lowpass_fast(n, 600)
    swell = 0.45 + 0.55 * (0.5 + 0.5 * np.sin(2 * np.pi * t / 7.0)) * (0.6 + 0.4 * np.sin(2 * np.pi * t / 2.9 + 1))
    s = n * swell * 0.6
    # gulls now and then
    for when in (3.0, 9.5, 15.0):
        i0 = int(when * SR)
        tt = t_axis(0.5)
        gull = np.sin(2 * np.pi * (1500 + 600 * np.sin(2 * np.pi * 3 * tt)) * tt) * np.exp(-tt * 5) * 0.12
        s[i0:i0 + len(gull)] += gull
    return seamless(s)


def amb_cafe(rng):
    length = 16.0
    t = t_axis(length)
    n = lowpass_fast(rng.r.normal(0, 1, len(t)).astype(np.float32), 300) * 0.25
    radio = lowpass_fast(rng.r.normal(0, 1, len(t)).astype(np.float32), 1800) * 0.05 * (0.5 + 0.5 * np.sin(2 * np.pi * 0.7 * t))
    clink = np.zeros_like(t)
    for when in (2.0, 6.3, 11.0):
        i0 = int(when * SR)
        tt = t_axis(0.3)
        clink[i0:i0 + len(tt)] += np.sin(2 * np.pi * 2400 * tt) * np.exp(-tt * 25) * 0.15
    return seamless(n + radio + clink)


def amb_wind(rng):
    length = 18.0
    t = t_axis(length)
    n = lowpass_fast(rng.r.normal(0, 1, len(t)).astype(np.float32), 400)
    gust = 0.3 + 0.7 * (0.5 + 0.5 * np.sin(2 * np.pi * t / 5.3)) ** 2
    return seamless(n * gust * 0.5)


def amb_clock(rng):
    length = 8.0
    t = t_axis(length)
    s = lowpass_fast(rng.r.normal(0, 1, len(t)).astype(np.float32), 200) * 0.06
    for k in range(8):
        i0 = int(k * SR)
        tt = t_axis(0.08)
        s[i0:i0 + len(tt)] += np.sin(2 * np.pi * (1800 if k % 2 else 1500) * tt) * np.exp(-tt * 60) * 0.3
    return seamless(s, 0.1)


def amb_radio(rng):
    length = 10.0
    t = t_axis(length)
    n = rng.r.normal(0, 1, len(t)).astype(np.float32)
    s = lowpass_fast(n, 2500) * 0.18 * (0.6 + 0.4 * np.sin(2 * np.pi * 0.3 * t))
    s += np.sin(2 * np.pi * 50 * t) * 0.03
    return seamless(s)


def sfx_page():
    t = t_axis(0.12)
    return lowpass_fast(np.random.default_rng(1).normal(0, 1, len(t)).astype(np.float32), 3000) * np.exp(-t * 40) * 0.4


def sfx_move():
    t = t_axis(0.08)
    return np.sin(2 * np.pi * 900 * t) * np.exp(-t * 50) * 0.35


def sfx_confirm():
    t = t_axis(0.25)
    return (np.sin(2 * np.pi * 660 * t) + 0.5 * np.sin(2 * np.pi * 990 * t)) * np.exp(-t * 14) * 0.35


def sfx_envelope():
    t = t_axis(0.5)
    n = np.random.default_rng(2).normal(0, 1, len(t)).astype(np.float32)
    return lowpass_fast(n, 4000) * np.exp(-t * 7) * (0.3 + 0.7 * np.clip(t * 12, 0, 1)) * 0.6


def sfx_stamp():
    t = t_axis(0.3)
    return (np.sin(2 * np.pi * 120 * t) * np.exp(-t * 25) + lowpass_fast(np.random.default_rng(3).normal(0, 1, len(t)).astype(np.float32), 1500) * np.exp(-t * 40) * 0.5) * 0.7


def sfx_horn():
    t = t_axis(2.5)
    env = np.minimum(np.minimum(t / 0.15, 1.0), (2.5 - t) / 0.6)
    s = np.zeros_like(t)
    for h, g in ((1, 1.0), (2, 0.5), (3, 0.3), (4, 0.2), (5, 0.1)):
        s += np.sin(2 * np.pi * 110 * h * t + 0.3 * np.sin(2 * np.pi * 3 * t)) * g
    return lowpass_fast(s * env, 1200) * 0.35


def sfx_motor():
    t = t_axis(1.5)
    s = np.sin(2 * np.pi * 48 * t) * 0.4 + np.sin(2 * np.pi * 96 * t) * 0.15
    return s * np.minimum(np.minimum(t / 0.2, 1.0), (1.5 - t) / 0.4)


def sfx_static():
    t = t_axis(0.6)
    return lowpass_fast(np.random.default_rng(4).normal(0, 1, len(t)).astype(np.float32), 3500) * np.exp(-t * 6) * 0.5


def sfx_bell():
    t = t_axis(1.8)
    return (np.sin(2 * np.pi * 1760 * t) + 0.6 * np.sin(2 * np.pi * 2637 * t) + 0.3 * np.sin(2 * np.pi * 4400 * t)) * np.exp(-t * 3) * 0.3


# ---- main -----------------------------------------------------------------------------------------------------

def main():
    TEX.mkdir(parents=True, exist_ok=True)
    rng = Rng(1)
    count = 0
    for loc, fn in LOCATIONS.items():
        for k, (name, img) in enumerate(fn()):
            save(grain(img, rng, 7), f"bg/{loc}_{k}_{name}")
            count += 1
    for name in CAST:
        for expr in EXPRESSIONS:
            for pose in POSES:
                save(doll(name, expr, pose), f"cast/{name}_{expr}_{pose}")
                count += 1
    save(paper_normal(), "paper_normal")
    for kind in ("warm", "neutral", "cold", "sepia", "night"):
        save(lut(kind), f"lut_{kind}")
    save(dialogue_box(), "ui/box")
    save(name_tag(), "ui/name_tag")
    save(choice_row(), "ui/choice_row")
    save(panel(), "ui/panel")
    save(letter_card(), "ui/letter_card")
    save(envelope(), "ui/envelope")
    save(stamp(), "ui/stamp")
    save(fade_quad(), "ui/fade")
    save(glow(), "ui/glow")
    count += 15
    arng = Rng(7)
    sounds = {
        "music/harbour": music_harbour(), "music/cafe": music_cafe(), "music/drawer": music_drawer(),
        "music/light": music_light(), "music/cove": music_cove(), "music/sunday": music_sunday(),
        "ambience/sea": amb_sea(arng), "ambience/cafe": amb_cafe(arng), "ambience/wind": amb_wind(arng),
        "ambience/clock": amb_clock(arng), "ambience/radio": amb_radio(arng),
        "sfx/page": sfx_page(), "sfx/move": sfx_move(), "sfx/confirm": sfx_confirm(), "sfx/envelope": sfx_envelope(),
        "sfx/stamp": sfx_stamp(), "sfx/horn": sfx_horn(), "sfx/motor": sfx_motor(), "sfx/static": sfx_static(),
        "sfx/bell": sfx_bell(),
    }
    for name, s in sounds.items():
        write_wav(AUD / f"{name}.wav", s.astype(np.float32))
    print(f"gen_assets: {count} textures under {TEX}, {len(sounds)} sounds under {AUD}")


if __name__ == "__main__":
    main()
