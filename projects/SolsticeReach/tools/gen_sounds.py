#!/usr/bin/env python3
"""Synthesizes Solstice Reach's sounds into assets/sfx/ (deterministic: same bytes every run).

    python3 projects/SolsticeReach/tools/gen_sounds.py

chime.wav  a pentatonic chord, a shard taken
flare.wav  a rising sweep, a mirror aligned
organ.wav  a long C-major chord, the finale

Mono 16-bit 48 kHz WAV, each with an .mceasset sidecar (importer "wav") whose GUID is derived
from the file name, so regenerating never moves a reference the scene holds.
"""

import json
import math
import pathlib
import struct
import uuid
import wave

OUT = pathlib.Path(__file__).resolve().parent.parent / "assets" / "sfx"
RATE = 48000


def sweep(seconds, hz0, hz1, gain, noise, decay):
    n = int(seconds * RATE)
    out = []
    phase = 0.0
    rng = 0x1234567
    for i in range(n):
        t = i / n
        phase += (hz0 + (hz1 - hz0) * t) / RATE
        rng = (rng * 1664525 + 1013904223) & 0xFFFFFFFF
        r = (rng >> 9) / float(1 << 23) * 2.0 - 1.0
        env = math.exp(-decay * t) * min(1.0, i / 96.0)
        out.append(gain * env * ((1.0 - noise) * math.sin(phase * 2.0 * math.pi) + noise * r))
    return out


def chord(seconds, hz, gain, decay):
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        v = sum(math.sin(t * f * 2.0 * math.pi) for f in hz) / len(hz)
        out.append(gain * v * math.exp(-decay * t / seconds) * min(1.0, i / 400.0))
    return out


def write(name, samples):
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"{name}.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(round(s * 32767))))) for s in samples))
    guid = str(uuid.uuid5(uuid.NAMESPACE_URL, f"mce:solstice_reach/sfx/{name}.wav"))
    sidecar = {"guid": guid, "importer": "wav", "importer_version": 1, "settings": {}}
    (OUT / f"{name}.wav.mceasset").write_text(json.dumps(sidecar, indent=2) + "\n", encoding="utf-8")
    print(f"gen_sounds: {path.name} {len(samples) / RATE:.2f} s guid {guid}")


def main():
    write("chime", chord(0.9, [523.25, 659.25, 783.99, 1046.5], 0.35, 5.0))
    write("flare", sweep(1.2, 300.0, 1400.0, 0.4, 0.1, 3.0))
    write("organ", chord(9.0, [130.81, 196.0, 261.63, 329.63, 392.0], 0.5, 1.5))


if __name__ == "__main__":
    main()
