#!/usr/bin/env python3
"""Fetches the CC0 sound sources of 09_openworld from OpenGameArt, checks their SHA-256 and writes assets/audio/.

    python3 samples/09_openworld/tools/fetch_audio.py          (needs ffmpeg and numpy)

The downloads are pinned: a changed upstream file fails the check, and its licence has to be read again before the pin
moves. The unmodified files (engine loop, footsteps) are copied as they are; the gun shots are trimmed, the siren is cut
to whole wail periods and the ambience to a crossfaded loop (ffmpeg and numpy only, no random input, so every run writes
the same bytes). Credits and sources: assets/audio/SOURCES.md and CREDITS.md. gen_assets.py then writes the sound
sets and the scene that use these files.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request
import wave
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "assets", "audio"))
BASE = "https://opengameart.org/sites/default/files/"

# name on OpenGameArt -> SHA-256 of the download (read 2026-10-29; every page says CC0)
SOURCES = {
    "loop_0.wav": "69d74b106509037ce547429e4c3f9ae906b0fffe88d0bb8b3b5a8b0cfc239048",
    "22 Pistol.wav": "32f12645134c1d8716f84ea984677ba0ae2cceab5fbc7db736c7754c3a9f4ebb",
    "22 Magnum.wav": "132a491a1e52c30042cac867ff461fc895c1a31b5420dfff24f5af0f2185d5cc",
    "siren.mp3": "fabe96ad2116d310b38acc143780e1c47c3cb27227deda13f84ce8e3cfb2248b",
    "crowd_shouting.ogg": "a2c23a64c127c77717cbe4abb21f138bac9d9aa6cb3cc89d62bab5d0d96dd7ca",
    "01-footstep.ogg": "33c9bef5e8aeb1069455699a34a0c5e1ef1787fd3f61594b0859d7e6bb9f9dec",
    "02-footstep.ogg": "f05396c807eb8eaecffb2a1ead611ec971c5a7914d1d081c91d7796e852692a2",
    "03-footstep.ogg": "9817cc93897ed1de5da87675b26be4196bfda3daa6f1de9dc4398a68d250cdb9",
    "04-footstep.ogg": "b373ff981e44d6f7ba2291225a4803078771029615ed81606136fdb9599906d5",
    "05-footstep.ogg": "f1b4893dd3cebe23438567a0dcaf047848a52b594e7eb03e5c212d99e83d2298",
    "06-footstep.ogg": "39147cc1b98abb4b5378ad07b15d50d18f2aa0f0fd2f5514f078c1e574d14773",
    "amb_rain_loop_1.ogg": "ae578791d46218d180a0e3aebefb5d621dbdd3f1627899c25210379f57d480b0",
    "wind background noise 2.wav": "9f1fd5c8b37949e6889a7f6acbd822f66e0ab4e918d72bda58d8d8c2c7e85a80",
    "crickets_1.mp3": "5b5bd1f744087888c5623543c3ff80be9c2ce106093993e7c8a8492b0280b2c0",
}
# One zip pack (rubberduck, "100 CC0 wood and metal SFX"): the members used here, copied unmodified
ZIP_NAME = "100-CC0-wood-metal-SFX.zip"
ZIP_SHA256 = "be6eba63b03409ac0c77787a956b1503a7c186403d04aef9725c52644a4b7878"
ZIP_MEMBERS = {"metal_hit_01.ogg": "hit_1.ogg", "metal_hit_02.ogg": "hit_2.ogg", "metal_hit_03.ogg": "hit_3.ogg",
               "metal_hit_04.ogg": "hit_4.ogg", "metal_hit_05.ogg": "hit_5.ogg", "metal_slam_01.ogg": "crash_1.ogg",
               "metal_falling_01.ogg": "crash_2.ogg", "metal_falling_02.ogg": "crash_3.ogg"}
RATE = 44100
BED_RATE = 22050   # the noise beds (rain, wind, crickets) have nothing above 8 kHz worth the bytes


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def fetch(cache):
    for name, want in list(SOURCES.items()) + [(ZIP_NAME, ZIP_SHA256)]:
        path = os.path.join(cache, name)
        if not (os.path.isfile(path) and sha256(path) == want):
            req = urllib.request.Request(BASE + urllib.parse.quote(name), headers={"User-Agent": "mce-content/1"})
            with urllib.request.urlopen(req, timeout=120) as r, open(path, "wb") as f:
                shutil.copyfileobj(r, f)
        got = sha256(path)
        if got != want:
            sys.exit("%s: sha256 is %s, pinned %s. The upstream file changed: read its licence again, then move the pin." % (name, got, want))


def decode(path, rate=RATE):
    """Mono float samples at `rate`."""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(rate), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype="<f4").astype(np.float64)


def write_wav(name, x, rate=RATE):
    pcm = np.round(np.clip(x, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(os.path.join(OUT, name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def shot(src, seconds):
    """The shot and its first tail: peak to -1 dBFS, a 150 ms fade out."""
    x = decode(src)[:int(RATE * seconds)]
    x = x / max(1e-9, np.abs(x).max()) * 0.89
    fade = int(RATE * 0.15)
    x[-fade:] *= np.linspace(1.0, 0.0, fade)
    return x


def siren(src, periods):
    """`periods` whole wail periods, cut where the pitch falls back (the centroid drops), with a 5 ms join."""
    x = decode(src)
    hop = RATE // 50
    n = len(x) // hop
    centroid = []
    freq = np.fft.rfftfreq(hop, 1.0 / RATE)
    win = np.hanning(hop)
    for i in range(n):
        s = np.abs(np.fft.rfft(x[i * hop:(i + 1) * hop] * win))
        centroid.append((s * freq).sum() / (s.sum() + 1e-9))
    drops = [i for i in range(60, n - 1) if centroid[i] - centroid[i + 1] > 300.0 and centroid[i + 1] < 1300.0]
    first = drops[0]
    last = next(d for d in drops if d - first >= periods * 34 - 3)
    seg = x[(first + 1) * hop:(last + 1) * hop].copy()
    ramp = int(RATE * 0.005)
    seg[:ramp] *= np.linspace(0.0, 1.0, ramp)
    seg[-ramp:] *= np.linspace(1.0, 0.0, ramp)
    return seg / max(1e-9, np.abs(seg).max()) * 0.89


def loop_of(src, seconds, fade, rate=RATE):
    """A crossfaded loop: the last `fade` seconds blend into the first."""
    x = decode(src, rate)[:int(rate * (seconds + fade))]
    f = int(rate * fade)
    n = len(x) - f
    ramp = np.linspace(0.0, 1.0, f)
    out = x[:n].copy()
    out[:f] = x[:f] * ramp + x[n:] * (1.0 - ramp)
    return out / max(1e-9, np.abs(out).max()) * 0.7


def main():
    cache = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.expanduser("~"), ".cache", "mce", "09_openworld_audio")
    os.makedirs(cache, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    fetch(cache)
    shutil.copyfile(os.path.join(cache, "loop_0.wav"), os.path.join(OUT, "engine_loop.wav"))
    for n in range(1, 7):
        shutil.copyfile(os.path.join(cache, "0%d-footstep.ogg" % n), os.path.join(OUT, "footstep_%d.ogg" % n))
    with zipfile.ZipFile(os.path.join(cache, ZIP_NAME)) as z:
        for member, out in ZIP_MEMBERS.items():
            with open(os.path.join(OUT, out), "wb") as f:
                f.write(z.read(member))
    write_wav("shot_1.wav", shot(os.path.join(cache, "22 Pistol.wav"), 0.9))
    write_wav("shot_2.wav", shot(os.path.join(cache, "22 Magnum.wav"), 1.1))
    write_wav("siren.wav", siren(os.path.join(cache, "siren.mp3"), 8))
    write_wav("city_ambience.wav", loop_of(os.path.join(cache, "crowd_shouting.ogg"), 14.0, 1.5))
    write_wav("rain_loop.wav", loop_of(os.path.join(cache, "amb_rain_loop_1.ogg"), 16.0, 2.0, BED_RATE), BED_RATE)
    write_wav("wind_loop.wav", loop_of(os.path.join(cache, "wind background noise 2.wav"), 16.0, 2.0, BED_RATE), BED_RATE)
    write_wav("night_crickets.wav", loop_of(os.path.join(cache, "crickets_1.mp3"), 10.0, 1.4, BED_RATE), BED_RATE)
    print("09_openworld audio: %d files in %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
