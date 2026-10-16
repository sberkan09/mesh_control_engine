#!/usr/bin/env python3
"""Regenerates cinema_demo's synthesized content: the intro music (assets/sfx/gate_theme.wav) and
the studio splash film (assets/video/splash.mpg, MPEG-1 + MP2 through ffmpeg). Deterministic: the
same bytes on every run with the same ffmpeg.

    python3 samples/cinema_demo/tools/gen_content.py
"""

import math
import os
import struct
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
RATE = 24000


def note(f):
    return 440.0 * 2.0 ** ((f - 69) / 12.0)


def gate_theme(path):
    """Eight seconds: a slow minor pad (Am, F, C, G) under a bell line, faded in and out."""
    seconds = 8.0
    chords = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]
    bells = [76, 72, 74, 71, 72, 69, 71, 67]
    n = int(RATE * seconds)
    out = bytearray()
    for i in range(n):
        t = i / RATE
        chord = chords[min(int(t / 2.0), 3)]
        s = 0.0
        for m in chord:
            f = note(m)
            s += 0.10 * math.sin(2 * math.pi * f * t) + 0.04 * math.sin(2 * math.pi * f * 2.003 * t)
        b = int(t / 1.0)
        bt = t - b
        s += 0.18 * math.sin(2 * math.pi * note(bells[b]) * t) * math.exp(-3.0 * bt)
        env = min(1.0, t / 1.5) * min(1.0, (seconds - t) / 1.5)
        v = max(-1.0, min(1.0, s * env))
        out += struct.pack("<h", int(v * 30000))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(out))


def splash(path, font):
    """Three seconds at 320x180, 25 fps: a dark ground, the title fading in, a low chime."""
    text = "drawtext=fontfile='{}':text='MESH CONTROL':fontsize=24:fontcolor=white:x=(w-tw)/2:y=(h-th)/2-10:alpha='min(1,t/1.2)'," \
           "drawtext=fontfile='{}':text='presents':fontsize=12:fontcolor=0xC8B48C:x=(w-tw)/2:y=(h/2)+18:alpha='max(0,min(1,(t-1)/0.8))'".format(font, font)
    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
           "-f", "lavfi", "-i", "color=c=0x101828:s=320x180:r=25:d=3",
           "-f", "lavfi", "-i", "sine=frequency=330:sample_rate=44100:duration=3",
           "-filter_complex", "[0:v]" + text + ",fade=t=out:st=2.4:d=0.6[v];[1:a]volume='0.4*exp(-1.4*t)':eval=frame[a]",
           "-map", "[v]", "-map", "[a]", "-c:v", "mpeg1video", "-q:v", "2", "-g", "12", "-bf", "0",
           "-c:a", "mp2", "-b:a", "96k", "-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact",
           "-f", "mpeg", path]
    subprocess.run(cmd, check=True)


def main():
    gate_theme(os.path.join(ASSETS, "sfx", "gate_theme.wav"))
    splash(os.path.join(ASSETS, "video", "splash.mpg"), os.path.join(ASSETS, "fonts", "ProggyClean.ttf"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
