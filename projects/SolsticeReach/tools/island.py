"""The island of Solstice Reach as numbers: the height field every tool and the game agree on.

`height(x, z)` is the terrain's surface height in metres (sea level 0) at world (x, z). The game
module ports this function line for line (games/solstice_reach/module/terrain.h) in double
precision with the same integer hash, so the generated render mesh, the authoring script's object
placement and the runtime collider all come from one definition. Keep the arithmetic order
identical in both files when changing either.

The districts (docs/design/solstice-reach.md §2.1) are plateaus (flat discs blended into the
field) joined by ramps (a flat band between two heights). Their constants live here too, so the
authoring script can place architecture on them.
"""

import math

SEA_FLOOR = -7.0
ISLAND_RADIUS = 92.0
HALF = 100.0  # the terrain mesh covers [-HALF, HALF]^2


def h01(ix, iz, seed):
    """uint32 lattice hash -> [0, 1)."""
    n = (ix * 374761393 + iz * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    n = (n ^ (n >> 13)) & 0xFFFFFFFF
    n = (n * 1274126177) & 0xFFFFFFFF
    n = n ^ (n >> 16)
    return n / 4294967296.0


def vnoise(x, z, seed):
    ix, iz = math.floor(x), math.floor(z)
    fx, fz = x - ix, z - iz
    sx = fx * fx * (3.0 - 2.0 * fx)
    sz = fz * fz * (3.0 - 2.0 * fz)
    a, b = h01(ix, iz, seed), h01(ix + 1, iz, seed)
    c, d = h01(ix, iz + 1, seed), h01(ix + 1, iz + 1, seed)
    top = a + (b - a) * sx
    bot = c + (d - c) * sx
    return top + (bot - top) * sz


def fbm(x, z, seed, octaves):
    v, amp, f = 0.0, 0.5, 1.0
    for o in range(octaves):
        v += amp * vnoise(x * f, z * f, seed + o * 131)
        amp *= 0.5
        f *= 2.03
    return v


def smooth(t):
    return t * t * (3.0 - 2.0 * t)


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


# (x, z, radius, height, blend width)
PLATEAUS = [
    (0.0, 74.0, 11.0, 1.6, 8.0),       # the Landing (south cove)
    (-28.0, 40.0, 11.0, 14.0, 7.0),    # Terraces: the windmill terrace
    (10.0, 44.0, 9.0, 9.0, 6.0),       # Terraces: lower terrace
    (54.0, 8.0, 15.0, 14.0, 8.0),      # the Cistern Garden (east hollow)
    (-56.0, 26.0, 7.0, 12.0, 5.0),     # Stair of Hours: foot
    (-66.0, -10.0, 6.0, 28.0, 5.0),    # Stair of Hours: first landing
    (-52.0, -42.0, 8.0, 40.0, 5.0),    # Stair of Hours: top
    (-4.0, -62.0, 16.0, 42.0, 9.0),    # the Colonnade (north ridge)
    (48.0, -44.0, 13.0, 30.0, 6.0),    # the Crystal Cut (quarry)
    (-6.0, -20.0, 12.0, 50.0, 8.0),    # the Vault (summit)
    (-38.0, 6.0, 4.5, 32.0, 3.0),      # the goat path's landing (where its two legs meet)
]

# (x0, z0, h0, x1, z1, h1, half width, blend width). Ramps are applied after the plateaus, so a
# ramp starts and ends a little inside the plateaus it joins (at their height) and the rise is
# spread over the whole gap; check_paths() below prints the steepest grade of each one.
RAMPS = [
    (0.0, 65.0, 1.6, 8.0, 50.0, 9.0, 3.0, 4.0),          # Landing -> lower terrace
    (4.0, 40.0, 9.0, -20.0, 43.0, 14.0, 3.0, 4.0),       # lower terrace -> windmill terrace
    (16.0, 41.0, 9.0, 44.0, 16.0, 14.0, 3.0, 4.0),       # lower terrace -> Cistern Garden
    (-36.0, 35.0, 14.0, -52.0, 28.0, 12.0, 3.0, 4.0),    # windmill terrace -> Stair foot
    (-57.0, 21.0, 12.0, -65.0, -6.0, 28.0, 2.5, 3.0),    # Stair: foot -> first landing
    (-65.0, -13.0, 28.0, -54.0, -37.0, 40.0, 2.5, 3.0),  # Stair: first landing -> top
    (-47.0, -45.0, 40.0, -18.0, -58.0, 42.0, 3.0, 4.0),  # Stair top -> Colonnade
    (-10.0, -49.0, 42.0, -8.0, -30.0, 50.0, 3.0, 4.0),   # Colonnade -> Vault
    (53.0, -4.0, 14.0, 50.0, -34.0, 30.0, 3.0, 4.0),     # Cistern Garden -> Crystal Cut
    (38.0, -49.0, 30.0, 10.0, -62.0, 42.0, 3.0, 4.0),    # Crystal Cut -> Colonnade
    (-22.0, 32.0, 14.0, -35.5, 9.5, 32.0, 2.5, 3.5),     # windmill terrace -> the goat path's landing
    (-40.0, 3.0, 32.0, -12.0, -10.0, 50.0, 2.5, 3.5),    # the goat path's landing -> Vault
]


def base_height(x, z):
    t = clamp((60.0 - z) / 150.0, 0.0, 1.0)
    h = 2.0 + 38.0 * smooth(t)
    h += (fbm(x * 0.012 + 7.0, z * 0.012 + 3.0, 11, 5) - 0.5) * 18.0
    h += (fbm(x * 0.05 + 1.0, z * 0.05 + 9.0, 23, 4) - 0.5) * 4.0
    # Coastline: a noisy radius, then a soft fall to the sea floor.
    d = math.sqrt(x * x + z * z)
    if d > 0.001:
        cx, cz = x / d, z / d
    else:
        cx, cz = 1.0, 0.0
    radius = ISLAND_RADIUS + (fbm(cx * 3.0 + 5.0, cz * 3.0 + 5.0, 41, 3) - 0.5) * 22.0
    fall = smooth(clamp((radius - d) / 24.0, 0.0, 1.0))
    h = h * fall + SEA_FLOOR * (1.0 - fall)
    # The north cliff: the ridge ends in a drop.
    cliff = smooth(clamp((-z - 80.0) / 12.0, 0.0, 1.0))
    h = h * (1.0 - cliff) + SEA_FLOOR * cliff
    return h


def grade(t):
    """A ramp's height profile: mostly linear with eased ends (steepest grade 1.15x the mean)."""
    return 0.7 * t + 0.3 * smooth(t)


def height(x, z):
    h = base_height(x, z)
    for (px, pz, pr, ph, blend) in PLATEAUS:
        dist = math.sqrt((x - px) * (x - px) + (z - pz) * (z - pz))
        w = smooth(clamp((pr + blend - dist) / blend, 0.0, 1.0))
        h = h * (1.0 - w) + ph * w
    for (x0, z0, h0, x1, z1, h1, hw, blend) in RAMPS:
        dx, dz = x1 - x0, z1 - z0
        length2 = dx * dx + dz * dz
        t = clamp(((x - x0) * dx + (z - z0) * dz) / length2, 0.0, 1.0)
        px, pz = x0 + dx * t, z0 + dz * t
        dist = math.sqrt((x - px) * (x - px) + (z - pz) * (z - pz))
        w = smooth(clamp((hw + blend - dist) / blend, 0.0, 1.0))
        target = h0 + (h1 - h0) * grade(t)
        h = h * (1.0 - w) + target * w
    return h


def normal(x, z, eps=0.5):
    hx = height(x + eps, z) - height(x - eps, z)
    hz = height(x, z + eps) - height(x, z - eps)
    n = (-hx, 2.0 * eps, -hz)
    l = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
    return (n[0] / l, n[1] / l, n[2] / l)


def check_paths(step=0.5):
    """The steepest grade (degrees) along each ramp's centre line, sampled every `step` metres."""
    out = []
    for (x0, z0, h0, x1, z1, h1, hw, blend) in RAMPS:
        length = math.hypot(x1 - x0, z1 - z0)
        n = max(2, int(length / step))
        worst = 0.0
        prev = height(x0, z0)
        for i in range(1, n + 1):
            t = i / n
            h = height(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t)
            worst = max(worst, math.degrees(math.atan2(abs(h - prev), length / n)))
            prev = h
        out.append(((x0, z0), (x1, z1), worst))
    return out


if __name__ == "__main__":
    for (a, b, worst) in check_paths():
        print("ramp %s -> %s steepest %4.1f deg" % (a, b, worst))
    for (px, pz, pr, ph, _) in PLATEAUS:
        print("plateau (%6.1f, %6.1f) r %4.1f -> %6.2f" % (px, pz, pr, height(px, pz)))
    print("probe", height(0.0, 0.0), height(30.0, -30.0), height(-80.0, 60.0), height(0.0, -95.0))
