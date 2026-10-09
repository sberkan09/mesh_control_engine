#!/usr/bin/env python3
"""Undertow's sounds: every WAV under assets/sfx is synthesized here (numpy, 48 kHz 16-bit mono).
Deterministic (seeded). The music loop is a slow chord pad over a sub pulse; the hum is the drone's
engine loop (gain driven by the game); the rest are one-shots.

    python3 projects/Undertow/tools/gen_sfx.py
"""

import pathlib
import struct
import wave

import numpy as np

OUT = pathlib.Path(__file__).resolve().parents[1] / "assets" / "sfx"
SR = 48000


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


def env(t, attack, decay, sustain=0.0, release=0.0, length=None):
    length = length if length is not None else t[-1]
    e = np.ones_like(t)
    e = np.where(t < attack, t / max(attack, 1e-5), e)
    d = np.clip((t - attack) / max(decay, 1e-5), 0, 1)
    e = np.where((t >= attack), (1 - d) * (1 - sustain) + sustain, e)
    if release > 0:
        r = np.clip((length - t) / release, 0, 1)
        e *= r
    return e


def lowpass(x, cutoff):
    rc = 1.0 / (2 * np.pi * cutoff)
    a = (1.0 / SR) / (rc + 1.0 / SR)
    y = np.zeros_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc += a * (x[i] - acc)
        y[i] = acc
    return y


def lowpass_fast(x, cutoff):
    # One-pole filter via a vectorized IIR (scipy-free): use a recursive cumulative trick is not
    # exact, so do a short python loop on chunks instead.
    return lowpass(x, cutoff)


def hum(rng):
    t = t_axis(1.0)
    base = 0.35 * np.sin(2 * np.pi * 55 * t) + 0.2 * np.sin(2 * np.pi * 110.5 * t) + 0.12 * np.sin(2 * np.pi * 164 * t)
    noise = rng.normal(0, 1, len(t)).astype(np.float32)
    noise = lowpass_fast(noise, 400) * 1.8
    s = base + noise * 0.5
    # Make it loop seamlessly: crossfade the tail into the head.
    n = int(SR * 0.05)
    ramp = np.linspace(0, 1, n, dtype=np.float32)
    s[-n:] = s[-n:] * (1 - ramp) + s[:n] * ramp
    return s * 0.6


def bump(rng):
    t = t_axis(0.18)
    s = np.sin(2 * np.pi * (80 + 60 * np.exp(-t * 30)) * t) * env(t, 0.002, 0.14)
    s += lowpass_fast(rng.normal(0, 1, len(t)).astype(np.float32), 900) * env(t, 0.001, 0.05) * 0.8
    return s * 0.8


def hit(rng):
    t = t_axis(0.45)
    noise = lowpass_fast(rng.normal(0, 1, len(t)).astype(np.float32), 2500) * env(t, 0.002, 0.25)
    tone = np.sin(2 * np.pi * (220 * np.exp(-t * 6)) * t) * env(t, 0.002, 0.3)
    crackle = np.sign(rng.normal(0, 1, len(t))) * (rng.random(len(t)) < 0.03) * env(t, 0.0, 0.3)
    return (noise * 1.2 + tone * 0.7 + crackle * 0.4) * 0.8


def explode(rng):
    t = t_axis(1.4)
    noise = lowpass_fast(rng.normal(0, 1, len(t)).astype(np.float32), 1200) * env(t, 0.005, 0.9)
    boom = np.sin(2 * np.pi * (48 + 30 * np.exp(-t * 4)) * t) * env(t, 0.005, 1.0)
    return (noise * 1.4 + boom * 1.0) * 0.75


def tether_on():
    t = t_axis(0.25)
    f = 300 + 900 * (t / t[-1])
    s = np.sin(2 * np.pi * f * t) * env(t, 0.005, 0.2)
    s += np.sin(2 * np.pi * f * 2 * t) * env(t, 0.005, 0.1) * 0.3
    return s * 0.6


def tether_off():
    t = t_axis(0.2)
    f = 900 - 600 * (t / t[-1])
    return np.sin(2 * np.pi * f * t) * env(t, 0.005, 0.15) * 0.5


def chime(notes, step=0.12, hold=0.5):
    length = step * len(notes) + hold
    t = t_axis(length)
    s = np.zeros_like(t)
    for i, f in enumerate(notes):
        start = i * step
        tt = np.clip(t - start, 0, None)
        gate = (t >= start).astype(np.float32)
        s += (np.sin(2 * np.pi * f * tt) + 0.35 * np.sin(2 * np.pi * f * 2 * tt) + 0.15 * np.sin(2 * np.pi * f * 3 * tt)) * \
             np.exp(-tt * 4.5) * gate
    return s * 0.45


def deliver():
    return chime([523.25, 659.25, 783.99, 1046.5, 1318.5], step=0.1, hold=0.9)


def pad_sound():
    return chime([392.0, 587.33], step=0.16, hold=0.6)


def switch_sound():
    t = t_axis(0.16)
    s = np.sign(np.sin(2 * np.pi * 1200 * t)) * 0.25 * env(t, 0.002, 0.1)
    s += np.sin(2 * np.pi * 600 * t) * 0.3 * env(t, 0.002, 0.14)
    return s


def win():
    return chime([523.25, 659.25, 783.99, 1046.5, 783.99, 1046.5, 1318.5, 1567.98], step=0.14, hold=1.6)


def music(rng):
    """A 16-second loop: four slow chords (Dm9 / Bbmaj7 / Gm7 / A7sus) on detuned saw-ish pads with a
    sub pulse, 60 bpm, 4 bars each 4 beats."""
    bar = 4.0
    t = t_axis(bar * 4)
    chords = [
        [146.83, 174.61, 220.0, 261.63, 329.63],   # D F A C E
        [116.54, 146.83, 174.61, 220.0, 261.63],   # Bb D F A C
        [98.0, 116.54, 146.83, 174.61, 220.0],     # G Bb D F A
        [110.0, 146.83, 164.81, 220.0, 261.63],    # A D E A C
    ]
    s = np.zeros_like(t)
    for i, chord in enumerate(chords):
        start, end = i * bar, (i + 1) * bar
        seg = (t >= start) & (t < end)
        tt = t[seg] - start
        fade = np.minimum(np.minimum(tt / 0.8, 1.0), (bar - tt) / 0.8)
        part = np.zeros_like(tt)
        for f in chord:
            for det in (-0.4, 0.0, 0.4):
                ff = f * (1 + det / 100.0)
                part += (np.sin(2 * np.pi * ff * tt) + 0.3 * np.sin(2 * np.pi * ff * 2 * tt + 0.5)) * 0.08
        # Slow tremolo.
        part *= 0.8 + 0.2 * np.sin(2 * np.pi * 0.25 * tt)
        s[seg] += part * fade
    # Sub pulse on each beat.
    beat = (t % 1.0)
    sub = np.sin(2 * np.pi * 36.7 * t) * np.exp(-beat * 5) * 0.35
    s += sub
    # Sparse high plinks on the off-beats.
    plink_t = t % 2.0
    plink = np.sin(2 * np.pi * 880 * t) * np.exp(-np.clip(plink_t - 1.0, 0, None) * 8) * (plink_t >= 1.0) * 0.05
    s += plink
    s = lowpass_fast(s.astype(np.float32), 2200)
    # Seamless loop.
    n = int(SR * 0.3)
    ramp = np.linspace(0, 1, n, dtype=np.float32)
    s[-n:] = s[-n:] * (1 - ramp) + s[:n] * ramp
    return s * 0.9


def main():
    rng = np.random.default_rng(11)
    files = {
        "hum.wav": hum(rng), "bump.wav": bump(rng), "hit.wav": hit(rng), "explode.wav": explode(rng),
        "tether_on.wav": tether_on(), "tether_off.wav": tether_off(), "deliver.wav": deliver(), "pad.wav": pad_sound(),
        "switch.wav": switch_sound(), "win.wav": win(), "music.wav": music(rng),
    }
    for name, s in files.items():
        write_wav(OUT / name, s.astype(np.float32))
    print(f"gen_sfx: {len(files)} clips -> {OUT}")


if __name__ == "__main__":
    main()
