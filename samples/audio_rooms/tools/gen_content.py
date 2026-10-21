#!/usr/bin/env python3
"""Regenerates audio_rooms' synthesized sounds: three footsteps, a music loop, a voice line and a
machine hum. Deterministic: the same bytes on every run (a fixed-seed noise generator).

    python3 samples/audio_rooms/tools/gen_content.py
"""

import math
import os
import random
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
RATE = 22050


def write(path, samples):
    with wave.open(os.path.join(ASSETS, path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples))


def footstep(seed, tone):
    """A short thud and scuff: decaying low sine under low-passed noise."""
    rng = random.Random(seed)
    n = int(RATE * 0.14)
    out, lp = [], 0.0
    for i in range(n):
        t = i / RATE
        env = math.exp(-t * 38.0)
        lp += 0.35 * (rng.uniform(-1.0, 1.0) - lp)
        out.append(env * (0.55 * math.sin(2 * math.pi * tone * t) + 0.5 * lp))
    return out


def note(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


def theme():
    """Four seconds that loop: two bars of a soft pad (C, Am) with a plucked line."""
    n = int(RATE * 4.0)
    chords = [(48, 55, 64), (45, 52, 60)]
    line = [72, 76, 79, 76, 69, 72, 76, 72]
    out = []
    for i in range(n):
        t = i / RATE
        s = 0.0
        for m in chords[int(t / 2.0) % 2]:
            s += 0.09 * math.sin(2 * math.pi * note(m) * t)
        k = int(t / 0.5)
        pt = t - k * 0.5
        s += 0.18 * math.exp(-pt * 7.0) * math.sin(2 * math.pi * note(line[k % 8]) * t)
        out.append(s)
    return out


def voice():
    """2.4 s of speech-like syllables: a gliding buzz through two formant-ish resonances."""
    n = int(RATE * 2.4)
    out, phase = [], 0.0
    y1 = y2 = z1 = z2 = 0.0
    for i in range(n):
        t = i / RATE
        f0 = 150.0 + 25.0 * math.sin(2 * math.pi * 1.3 * t)
        phase += f0 / RATE
        saw = 2.0 * (phase % 1.0) - 1.0
        syl = t * 4.0
        env = max(0.0, math.sin(math.pi * (syl % 1.0))) ** 0.6 if int(syl) % 5 != 4 else 0.0
        # Two resonators (700 Hz and 1200 Hz) as two-pole band-passes.
        for f, state in ((700.0, 0), (1200.0, 1)):
            r = 0.97
            c = 2 * r * math.cos(2 * math.pi * f / RATE)
            if state == 0:
                y = saw + c * y1 - r * r * y2
                y2, y1 = y1, y
            else:
                z = saw + c * z1 - r * r * z2
                z2, z1 = z1, z
        out.append(env * 0.012 * (y1 + 0.7 * z1))
    return out


def hum():
    """One second that loops: 100 Hz mains with harmonics and a 2.5 kHz whine (whole cycles)."""
    n = RATE
    return [0.25 * math.sin(2 * math.pi * 100 * i / RATE) + 0.12 * math.sin(2 * math.pi * 300 * i / RATE)
            + 0.08 * math.sin(2 * math.pi * 2500 * i / RATE) for i in range(n)]


def main():
    for k, (seed, tone) in enumerate(((1, 70.0), (2, 82.0), (3, 64.0)), start=1):
        write("sfx/step_%d.wav" % k, footstep(seed, tone))
    write("music/theme.wav", theme())
    write("voice/line.wav", voice())
    write("sfx/hum.wav", hum())


if __name__ == "__main__":
    main()
