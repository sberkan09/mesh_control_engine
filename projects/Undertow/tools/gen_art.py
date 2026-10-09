#!/usr/bin/env python3
"""Undertow's art: every texture under assets/textures is generated here (numpy -> PNG), with
tangent-space normal maps for the sprites Light2D lights (rock, drone, crusher), the particle
sprites, the dock, and the colour-grade LUT. Deterministic (seeded), so a re-run is a no-op diff.

    python3 projects/Undertow/tools/gen_art.py          # writes assets/textures/*.png

The .mceasset sidecars are NOT written here: the MCP server's cook_run {"adopt": true} registers
new files (the agent's authoring script does that), which is the path a person's editor takes.
Normal maps and the LUT need "srgb": false in their sidecar; the authoring script patches that.
"""

import pathlib
import struct
import zlib

import numpy as np

OUT = pathlib.Path(__file__).resolve().parents[1] / "assets" / "textures"


def write_png(path, rgba):
    h, w, _ = rgba.shape
    raw = b"".join(b"\x00" + rgba[y].astype(np.uint8).tobytes() for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def canvas(w, h, color=(0, 0, 0, 0)):
    img = np.zeros((h, w, 4), dtype=np.float32)
    img[:, :] = color
    return img


def grid(w, h):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return x + 0.5, y + 0.5


def normal_map(height, strength=2.0, wrap=True):
    """Tangent-space normals from a height field (x right, y up in texture space = row 0 is the top
    of the image, so dy is flipped)."""
    if wrap:
        dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
        dy = (np.roll(height, 1, axis=0) - np.roll(height, -1, axis=0)) * 0.5
    else:
        dx = np.gradient(height, axis=1)
        dy = -np.gradient(height, axis=0)
    n = np.stack([-dx * strength, -dy * strength, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    rgba = np.empty(height.shape + (4,), dtype=np.float32)
    rgba[..., :3] = (n * 0.5 + 0.5) * 255.0
    rgba[..., 3] = 255.0
    return rgba


def to_u8(img):
    return np.clip(img, 0, 255)


def disc(x, y, cx, cy, r):
    return np.clip(r - np.sqrt((x - cx) ** 2 + (y - cy) ** 2) + 0.5, 0, 1)


def rock(rng, size=32):
    """Tileable cobbled rock: a height field of overlapping stones, shaded, plus its normal map."""
    x, y = grid(size, size)
    height = np.zeros((size, size), dtype=np.float32)
    albedo = canvas(size, size, (0, 0, 0, 255))
    base = np.array([62, 70, 84], dtype=np.float32)
    albedo[..., :3] = base
    stones = []
    for _ in range(16):
        cx, cy, r = rng.uniform(0, size), rng.uniform(0, size), rng.uniform(4.5, 9.0)
        stones.append((cx, cy, r))
    for cx, cy, r in stones:
        for ox in (-size, 0, size):
            for oy in (-size, 0, size):
                d = np.sqrt((x - cx - ox) ** 2 + (y - cy - oy) ** 2)
                dome = np.clip(1.0 - (d / r) ** 2, 0, 1) ** 0.7
                height = np.maximum(height, dome * r * 0.45)
                tint = rng.uniform(0.7, 1.4)
                mask = dome > 0.02
                albedo[mask, :3] = np.maximum(albedo[mask, :3], (base * tint + dome[mask][:, None] * 28.0))
    noise = rng.normal(0, 1.0, (size, size)).astype(np.float32)
    height += noise * 0.08
    albedo[..., :3] += noise[..., None] * 4.0
    # Cracks between stones darker.
    low = height < 1.0
    albedo[low, :3] *= 0.45
    albedo[..., :3] = np.clip(albedo[..., :3], 10, 200)
    # A faint teal moss on the top-facing slopes.
    albedo[..., 1] += np.clip(height, 0, 3) * 3.0
    return to_u8(albedo), normal_map(height, 1.6)


def rock_fill():
    img = canvas(8, 8, (6, 7, 10, 255))
    return to_u8(img)


def cave_far(rng, w=256, h=128):
    x, y = grid(w, h)
    img = canvas(w, h)
    t = y / h  # 0 top .. 1 bottom
    img[..., 0] = 10 + 6 * (1 - t)
    img[..., 1] = 14 + 10 * (1 - t)
    img[..., 2] = 24 + 16 * (1 - t)
    img[..., 3] = 255
    # Stalactite silhouettes hanging from the top, stalagmites from the bottom.
    for _ in range(18):
        cx, width, length = rng.uniform(0, w), rng.uniform(6, 20), rng.uniform(20, 70)
        m = np.clip(1 - np.abs(x - cx) / (width * np.clip(1 - y / length, 0.05, 1)), 0, 1) * (y < length)
        img[m > 0, :3] *= 0.55
    for _ in range(14):
        cx, width, length = rng.uniform(0, w), rng.uniform(8, 26), rng.uniform(16, 50)
        yy = h - y
        m = np.clip(1 - np.abs(x - cx) / (width * np.clip(1 - yy / length, 0.05, 1)), 0, 1) * (yy < length)
        img[m > 0, :3] *= 0.5
    return to_u8(img)


def drone():
    s = 32
    x, y = grid(s, s)
    cx, cy = s / 2, s / 2
    img = canvas(s, s)
    height = np.zeros((s, s), dtype=np.float32)
    # Hull: a rounded capsule, wider than tall.
    body = np.clip(1 - np.sqrt(((x - cx) / 13.0) ** 2 + ((y - cy) / 8.5) ** 2), 0, 1)
    hull = body > 0
    img[hull, :3] = (120, 134, 150)
    img[hull, 3] = 255
    height += body * 6
    # Darker underside, rim light on top.
    img[..., :3] *= (1.0 + 0.25 * (1 - (y / s)))[..., None]
    # Cockpit dome.
    dome = np.clip(1 - np.sqrt(((x - cx) / 5.5) ** 2 + ((y - cy + 1.5) / 4.5) ** 2), 0, 1)
    dm = dome > 0
    img[dm, 0] = 90 + 120 * dome[dm]
    img[dm, 1] = 190 + 50 * dome[dm]
    img[dm, 2] = 230
    height += dome * 4
    # Side thruster pods.
    for px in (cx - 11, cx + 11):
        pod = disc(x, y, px, cy + 1, 3.5)
        pm = pod > 0
        img[pm, :3] = (70, 78, 92)
        img[pm, 3] = 255
        height += pod * 3
        nozzle = disc(x, y, px, cy + 4.5, 1.8)
        nm = nozzle > 0
        img[nm, :3] = (255, 170, 80)
        img[nm, 3] = 255
    # Headlamp.
    lamp = disc(x, y, cx, cy - 6.5, 2.2)
    lm = lamp > 0
    img[lm, :3] = (255, 245, 200)
    img[lm, 3] = 255
    # Rivets.
    for rx in (cx - 6, cx + 6):
        r = disc(x, y, rx, cy + 4, 0.9)
        img[r > 0, :3] = (60, 66, 78)
    nmap = normal_map(height, 1.3, wrap=False)
    nmap[..., 3] = img[..., 3]
    return to_u8(img), nmap


def pod():
    s = 24
    x, y = grid(s, s)
    c = s / 2
    img = canvas(s, s)
    orb = disc(x, y, c, c, 9.5)
    m = orb > 0
    d = np.sqrt((x - c) ** 2 + (y - c) ** 2) / 9.5
    img[m, 0] = 150 + 90 * (1 - d[m])
    img[m, 1] = 220 + 30 * (1 - d[m])
    img[m, 2] = 255
    img[m, 3] = 255
    ring = np.abs(d - 0.78) < 0.09
    img[ring, :3] = (40, 60, 80)
    band = (np.abs(y - c) < 1.6) & m
    img[band, :3] = (70, 90, 110)
    core = disc(x, y, c, c, 3.2)
    img[core > 0, :3] = (255, 255, 255)
    return to_u8(img)


def dock():
    w, h = 128, 32
    x, y = grid(w, h)
    img = canvas(w, h)
    deck = (y > 14) & (y < 26) & (x > 4) & (x < w - 4)
    img[deck, :3] = (60, 70, 84)
    img[deck, 3] = 255
    top = (y > 14) & (y < 17) & (x > 4) & (x < w - 4)
    img[top, :3] = (110, 125, 140)
    legs = ((np.abs(x - 20) < 3) | (np.abs(x - w + 20) < 3) | (np.abs(x - w / 2) < 3)) & (y >= 26)
    img[legs, :3] = (40, 46, 56)
    img[legs, 3] = 255
    # Slot markers along the deck.
    for i in range(5):
        sx = w / 2 + (i - 2) * 22
        slot = (np.abs(x - sx) < 6) & (y > 17) & (y < 19)
        img[slot, :3] = (120, 220, 200)
    # Antenna mast.
    mast = (np.abs(x - w / 2) < 1.2) & (y < 15) & (y > 2)
    img[mast, :3] = (90, 100, 115)
    img[mast, 3] = 255
    tip = disc(x, y, w / 2, 3, 2.2)
    img[tip > 0, :3] = (255, 120, 110)
    img[tip > 0, 3] = 255
    return to_u8(img)


def relay():
    w, h = 48, 16
    x, y = grid(w, h)
    img = canvas(w, h)
    deck = (y > 6) & (y < 12) & (x > 2) & (x < w - 2)
    img[deck, :3] = (58, 68, 82)
    img[deck, 3] = 255
    top = (y > 6) & (y < 8) & (x > 2) & (x < w - 2)
    img[top, :3] = (150, 200, 190)
    legs = ((np.abs(x - 8) < 2) | (np.abs(x - w + 8) < 2)) & (y >= 12)
    img[legs, :3] = (40, 46, 56)
    img[legs, 3] = 255
    return to_u8(img)


def beacon():
    s = 12
    x, y = grid(s, s)
    img = canvas(s, s)
    d = disc(x, y, s / 2, s / 2, 5)
    img[d > 0, :3] = (255, 255, 255)
    img[..., 3] = d * 255
    return to_u8(img)


def crystal():
    w, h = 24, 32
    x, y = grid(w, h)
    img = canvas(w, h)
    # A tall hexagonal shard, lighter on the left facet.
    half = 7 * np.clip(1 - np.abs(y - 18) / 16.0, 0, 1) + 2
    shard = (np.abs(x - w / 2) < half) & (y > 2)
    img[shard, :3] = (140, 230, 255)
    img[shard, 3] = 255
    left = shard & (x < w / 2)
    img[left, :3] = (200, 250, 255)
    edge = shard & (np.abs(np.abs(x - w / 2) - half) < 1.0)
    img[edge, :3] = (90, 150, 200)
    return to_u8(img)


def mine():
    s = 24
    x, y = grid(s, s)
    c = s / 2
    img = canvas(s, s)
    body = disc(x, y, c, c, 7)
    img[body > 0, :3] = (70, 60, 70)
    img[body > 0, 3] = 255
    for ang in np.linspace(0, 2 * np.pi, 8, endpoint=False):
        sx, sy = c + np.cos(ang) * 9.5, c + np.sin(ang) * 9.5
        spike = disc(x, y, sx, sy, 2.2)
        img[spike > 0, :3] = (110, 95, 100)
        img[spike > 0, 3] = 255
    eye = disc(x, y, c, c, 2.5)
    img[eye > 0, :3] = (255, 60, 60)
    return to_u8(img)


def gate(vertical=True):
    w, h = (8, 32) if vertical else (32, 8)
    x, y = grid(w, h)
    img = canvas(w, h)
    axis = x if vertical else y
    c = (w if vertical else h) / 2
    core = np.clip(1 - np.abs(axis - c) / 3.5, 0, 1)
    img[..., 0] = 255
    img[..., 1] = 40 + 160 * core ** 2
    img[..., 2] = 30 + 90 * core ** 2
    img[..., 3] = core ** 1.5 * 255
    return to_u8(img)


def switch():
    s = 24
    x, y = grid(s, s)
    img = canvas(s, s)
    base = (y > 15) & (x > 3) & (x < s - 3)
    img[base, :3] = (60, 66, 78)
    img[base, 3] = 255
    btn = disc(x, y, s / 2, 12, 7)
    img[btn > 0, :3] = (255, 255, 255)  # tinted by the game
    img[btn > 0, 3] = 255
    rim = (disc(x, y, s / 2, 12, 7) > 0) & (disc(x, y, s / 2, 12, 5.5) <= 0)
    img[rim, :3] = (40, 44, 52)
    return to_u8(img)


def crusher():
    w, h = 48, 24
    x, y = grid(w, h)
    img = canvas(w, h, (0, 0, 0, 255))
    height = np.zeros((h, w), dtype=np.float32)
    img[..., :3] = (84, 76, 70)
    plate = (y < 18)
    img[plate, :3] = (96, 88, 80)
    height += plate * 3
    for tx in range(4, w, 8):
        tooth = np.clip(1 - np.abs(x - tx - 2) / 3.0, 0, 1) * (y > 14)
        tm = tooth > 0.2
        img[tm, :3] = (150, 140, 120)
        height += tooth * 4 * (y > 14)
    stripe = ((x + y) % 12 < 6) & (y < 6)
    img[stripe, :3] = (200, 160, 40)
    return to_u8(img), normal_map(height, 1.2, wrap=False)


def dot(size=16, power=1.6):
    x, y = grid(size, size)
    c = size / 2
    d = np.sqrt((x - c) ** 2 + (y - c) ** 2) / (c - 0.5)
    a = np.clip(1 - d, 0, 1) ** power
    img = canvas(size, size, (255, 255, 255, 0))
    img[..., 3] = a * 255
    return to_u8(img)


def bubble(size=12):
    x, y = grid(size, size)
    c = size / 2
    d = np.sqrt((x - c) ** 2 + (y - c) ** 2) / (c - 0.5)
    ring = np.clip(1 - np.abs(d - 0.75) / 0.25, 0, 1)
    img = canvas(size, size, (255, 255, 255, 0))
    img[..., 3] = ring * 220
    return to_u8(img)


def white():
    return to_u8(canvas(4, 4, (255, 255, 255, 255)))


def lut_teal():
    """A 256x16 colour-grade strip (16 slices of 16x16): cool teal shadows, warm highlights, a touch
    more contrast. Imported with srgb off."""
    img = np.zeros((16, 256, 4), dtype=np.float32)
    for b in range(16):
        for g in range(16):
            for r in range(16):
                rr, gg, bb = r / 15.0, g / 15.0, b / 15.0
                lum = 0.3 * rr + 0.59 * gg + 0.11 * bb
                # Contrast curve.
                def curve(v):
                    return np.clip((v - 0.5) * 1.12 + 0.5, 0, 1)
                rr, gg, bb = curve(rr), curve(gg), curve(bb)
                shadow = (1 - lum) ** 2
                rr = np.clip(rr - 0.05 * shadow + 0.04 * lum, 0, 1)
                gg = np.clip(gg + 0.02 * shadow + 0.01 * lum, 0, 1)
                bb = np.clip(bb + 0.08 * shadow - 0.03 * lum, 0, 1)
                img[g, b * 16 + r] = (rr * 255, gg * 255, bb * 255, 255)
    return to_u8(img)


def main():
    rng = np.random.default_rng(7)
    rock_a, rock_n = rock(rng)
    drone_a, drone_n = drone()
    crusher_a, crusher_n = crusher()
    files = {
        "rock.png": rock_a, "rock_n.png": rock_n, "rock_fill.png": rock_fill(), "cave_far.png": cave_far(rng),
        "drone.png": drone_a, "pod.png": pod(), "dock.png": dock(), "relay.png": relay(),
        "beacon.png": beacon(), "crystal.png": crystal(), "mine.png": mine(), "gate_v.png": gate(True),
        "gate_h.png": gate(False), "switch.png": switch(), "crusher.png": crusher_a, "crusher_n.png": crusher_n,
        "dot.png": dot(), "spark.png": dot(8, 0.8), "bubble.png": bubble(), "white.png": white(), "lut_teal.png": lut_teal(),
    }
    for name, img in files.items():
        write_png(OUT / name, img)
    print(f"gen_art: {len(files)} textures -> {OUT}")


if __name__ == "__main__":
    main()
