#include "art.h"

#include <cmath>

namespace platformer::art {

using mce::i32;
using mce::u64;
using mce::usize;

namespace {

struct Rgba {
    u8 r, g, b, a;
};
constexpr Rgba kClear{0, 0, 0, 0};

struct Painter {
    Array<u8>& px;
    u32 layer;

    void set(i32 x, i32 y, Rgba c) const {
        if (x < 0 || y < 0 || x >= i32(kTexels) || y >= i32(kTexels)) return;
        u8* p = px.data() + ((usize(layer) * kTexels + usize(y)) * kTexels + usize(x)) * 4;
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
    }
    void rect(i32 x0, i32 y0, i32 x1, i32 y1, Rgba c) const {  // inclusive
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x) set(x, y, c);
    }
    void ellipse(f32 cx, f32 cy, f32 rx, f32 ry, Rgba c) const {
        for (i32 y = 0; y < i32(kTexels); ++y)
            for (i32 x = 0; x < i32(kTexels); ++x) {
                const f32 dx = (f32(x) + 0.5f - cx) / rx, dy = (f32(y) + 0.5f - cy) / ry;
                if (dx * dx + dy * dy <= 1.0f) set(x, y, c);
            }
    }
};

u32 lcg(u64& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>(s >> 33);
}

void hero(const Painter& p, int pose) {
    const Rgba skin{255, 206, 150, 255}, hair{110, 60, 30, 255}, shirt{225, 60, 55, 255}, pants{50, 70, 170, 255},
        boot{60, 40, 30, 255}, eye{20, 20, 30, 255};
    p.rect(5, 1, 10, 2, hair);
    p.rect(5, 3, 10, 7, skin);
    p.rect(4, 2, 4, 4, hair);
    p.set(9, 4, eye);
    p.rect(4, 8, 11, 11, shirt);
    p.rect(3, 9, 3, 11, skin);
    p.rect(12, 9, 12, 11, skin);
    p.rect(5, 12, 10, 12, pants);
    switch (pose) {
        case 0:  // idle
            p.rect(5, 13, 6, 14, pants);
            p.rect(9, 13, 10, 14, pants);
            p.rect(5, 15, 6, 15, boot);
            p.rect(9, 15, 10, 15, boot);
            break;
        case 1:  // stride
            p.rect(3, 13, 5, 14, pants);
            p.rect(10, 13, 12, 14, pants);
            p.rect(2, 15, 4, 15, boot);
            p.rect(11, 15, 13, 15, boot);
            break;
        case 2:  // passing
            p.rect(6, 13, 9, 14, pants);
            p.rect(6, 15, 9, 15, boot);
            break;
        default:  // jump: knees up
            p.rect(4, 13, 6, 13, pants);
            p.rect(9, 12, 11, 13, pants);
            p.rect(4, 14, 5, 14, boot);
            p.rect(11, 14, 12, 14, boot);
            p.rect(1, 7, 3, 8, skin);  // arms up
            p.rect(12, 7, 14, 8, skin);
            break;
    }
}

void blob(const Painter& p, bool angry) {
    const Rgba body = angry ? Rgba{215, 70, 70, 255} : Rgba{140, 80, 190, 255};
    const Rgba belly = angry ? Rgba{240, 130, 110, 255} : Rgba{185, 140, 225, 255};
    p.ellipse(8.0f, 10.0f, 7.5f, 6.0f, body);
    p.ellipse(8.0f, 12.0f, 4.5f, 3.0f, belly);
    p.rect(3, 15, 5, 15, Rgba{60, 30, 70, 255});
    p.rect(10, 15, 12, 15, Rgba{60, 30, 70, 255});
    p.rect(4, 7, 6, 9, Rgba{255, 255, 255, 255});
    p.rect(9, 7, 11, 9, Rgba{255, 255, 255, 255});
    p.rect(5, 8, 6, 9, Rgba{20, 20, 30, 255});
    p.rect(10, 8, 11, 9, Rgba{20, 20, 30, 255});
    if (angry) {
        p.rect(3, 5, 6, 5, Rgba{40, 10, 10, 255});
        p.rect(9, 5, 12, 5, Rgba{40, 10, 10, 255});
        p.set(6, 6, Rgba{40, 10, 10, 255});
        p.set(9, 6, Rgba{40, 10, 10, 255});
    }
}

void coin(const Painter& p, f32 rx) {
    p.ellipse(8.0f, 8.0f, rx, 7.0f, Rgba{205, 140, 20, 255});
    if (rx > 1.6f) p.ellipse(8.0f, 8.0f, rx - 1.2f, 5.8f, Rgba{252, 205, 45, 255});
    if (rx > 3.0f) p.rect(7, 4, 7, 10, Rgba{255, 245, 170, 255});
}

void dirt(const Painter& p, u64 seed) {
    u64 s = seed;
    for (i32 y = 0; y < 16; ++y)
        for (i32 x = 0; x < 16; ++x) {
            const u32 n = lcg(s) % 7;
            const u8 k = static_cast<u8>(n == 0 ? 20 : 0);
            p.set(x, y, Rgba{static_cast<u8>(140 - k), static_cast<u8>(92 - k), static_cast<u8>(52 - k / 2), 255});
        }
    s = seed ^ 0xABCDu;
    for (int i = 0; i < 4; ++i) {
        const i32 x = i32(lcg(s) % 15), y = i32(lcg(s) % 15);
        p.rect(x, y, x + 1, y, Rgba{170, 150, 130, 255});
    }
}

}  // namespace

void make_atlas(Array<u8>& out) {
    out.clear();
    out.resize(usize(kTexels) * kTexels * 4 * LayerCount);
    for (usize i = 0; i < out.size(); ++i) out[i] = 0;
    auto layer = [&out](Layer l) { return Painter{out, l}; };

    hero(layer(PlayerIdle), 0);
    hero(layer(PlayerRunA), 1);
    hero(layer(PlayerRunB), 2);
    hero(layer(PlayerJump), 3);
    blob(layer(EnemyCalm), false);
    blob(layer(EnemyAngry), true);
    coin(layer(Coin0), 6.5f);
    coin(layer(Coin1), 4.5f);
    coin(layer(Coin2), 1.5f);
    coin(layer(Coin3), 4.5f);

    {
        const Painter p = layer(Spike);
        for (i32 t = 0; t < 3; ++t) {
            const f32 cx = 2.75f + 5.25f * f32(t);
            for (i32 y = 0; y < 16; ++y) {
                const f32 half = 2.6f * f32(y + 1) / 16.0f;
                for (i32 x = 0; x < 16; ++x) {
                    const f32 d = f32(x) + 0.5f - cx;
                    if (d >= -half && d <= half) p.set(x, y, d < 0 ? Rgba{225, 225, 235, 255} : Rgba{150, 150, 165, 255});
                }
            }
        }
    }
    {
        const Painter p = layer(Flag);
        p.rect(6, 0, 7, 15, Rgba{230, 230, 230, 255});
        p.rect(5, 0, 8, 0, Rgba{250, 210, 60, 255});
        for (i32 y = 1; y <= 7; ++y) {
            const i32 w = 7 - std::abs(y - 4) * 2;
            p.rect(8, y, 8 + w, y, Rgba{60, 200, 95, 255});
        }
        p.rect(4, 15, 9, 15, Rgba{120, 120, 120, 255});
    }
    {
        const Painter p = layer(Platform);
        p.rect(0, 0, 15, 15, Rgba{165, 110, 55, 255});
        p.rect(0, 0, 15, 2, Rgba{205, 150, 85, 255});
        p.rect(0, 13, 15, 15, Rgba{105, 65, 30, 255});
        p.rect(0, 0, 0, 15, Rgba{105, 65, 30, 255});
        p.rect(15, 0, 15, 15, Rgba{105, 65, 30, 255});
        p.rect(7, 4, 8, 11, Rgba{125, 80, 40, 255});
    }
    {
        const Painter p = layer(GroundTop);
        dirt(p, 0x1234);
        p.rect(0, 0, 15, 3, Rgba{90, 185, 65, 255});
        p.rect(0, 0, 15, 0, Rgba{140, 220, 95, 255});
        for (i32 x = 0; x < 16; ++x) p.set(x, 4 + (x * 7 % 3 == 0 ? 1 : 0), Rgba{60, 140, 50, 255});
        for (i32 x = 0; x < 16; x += 3) p.set(x, 4, Rgba{60, 140, 50, 255});
    }
    dirt(layer(GroundFill), 0x9876);
    {
        const Painter p = layer(Plank);
        p.rect(0, 0, 15, 3, Rgba{190, 135, 70, 255});
        p.rect(0, 0, 15, 0, Rgba{225, 175, 105, 255});
        p.rect(0, 3, 15, 3, Rgba{120, 80, 40, 255});
        p.rect(2, 4, 3, 9, Rgba{120, 80, 40, 255});
        p.rect(12, 4, 13, 9, Rgba{120, 80, 40, 255});
    }
    layer(White).rect(0, 0, 15, 15, Rgba{255, 255, 255, 255});
    {
        const Painter p = layer(Cloud);
        const Rgba c{255, 255, 255, 235};
        p.ellipse(5.0f, 10.0f, 4.5f, 3.5f, c);
        p.ellipse(9.0f, 7.5f, 5.0f, 4.5f, c);
        p.ellipse(12.0f, 10.5f, 3.5f, 3.0f, c);
        p.rect(2, 11, 14, 12, c);
    }
    {
        const Painter p = layer(Hill);
        p.ellipse(8.0f, 16.0f, 8.0f, 14.0f, Rgba{120, 190, 120, 255});
    }
    {
        // Wooden crate: dark frame, planks, a diagonal brace.
        const Painter p = layer(Crate);
        const Rgba frame{110, 70, 35, 255}, plank{190, 135, 70, 255}, light{220, 170, 100, 255};
        p.rect(0, 0, 15, 15, plank);
        for (i32 y = 4; y < 12; y += 4) p.rect(2, y, 13, y, Rgba{160, 110, 55, 255});
        p.rect(0, 0, 15, 1, frame);
        p.rect(0, 14, 15, 15, frame);
        p.rect(0, 0, 1, 15, frame);
        p.rect(14, 0, 15, 15, frame);
        for (i32 i = 2; i < 14; ++i) {
            p.set(i, 15 - i, frame);
            p.set(i, 14 - i, frame);
        }
        p.rect(2, 2, 13, 2, light);
    }
    (void)kClear;
}

// ---- sound -----------------------------------------------------------------------------------------

namespace {

constexpr f32 kRate = 48000.0f;

usize frames(f32 seconds) { return static_cast<usize>(seconds * kRate); }

f32 square(f32 phase) { return phase - std::floor(phase) < 0.5f ? 1.0f : -1.0f; }
f32 triangle(f32 phase) {
    const f32 t = phase - std::floor(phase);
    return 4.0f * std::fabs(t - 0.5f) - 1.0f;
}
f32 midi_hz(i32 note) { return 440.0f * std::pow(2.0f, f32(note - 69) / 12.0f); }

// Short linear attack, exponential decay; zero at the end so nothing clicks.
f32 env(usize i, usize n, f32 decay) {
    const f32 t = f32(i) / kRate;
    const f32 a = t < 0.004f ? t / 0.004f : 1.0f;
    const f32 tail = 1.0f - f32(i) / f32(n);
    return a * std::exp(-decay * t) * tail;
}

// Frequency sweep from f0 to f1 (exponential), square or noise-mixed.
void sweep(Array<f32>& out, f32 seconds, f32 f0, f32 f1, f32 gain, f32 noise, f32 decay) {
    const usize n = frames(seconds);
    out.resize(n);
    u64 s = 0xC0FFEE;
    f32 phase = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const f32 k = f32(i) / f32(n);
        phase += f0 * std::pow(f1 / f0, k) / kRate;
        const f32 nz = f32(lcg(s) % 2001) / 1000.0f - 1.0f;
        out[i] = gain * env(i, n, decay) * ((1.0f - noise) * square(phase) + noise * nz);
    }
}

// A sequence of square notes (pickup chime, win fanfare).
void notes(Array<f32>& out, const i32* midi, usize count, f32 note_s, f32 gain) {
    const usize per = frames(note_s);
    out.resize(per * count);
    for (usize n = 0; n < count; ++n) {
        const f32 hz = midi_hz(midi[n]);
        for (usize i = 0; i < per; ++i) {
            const f32 last = n + 1 == count ? 1.0f : 0.0f;
            out[n * per + i] = gain * env(i, per, 6.0f + 10.0f * (1.0f - last)) * square(hz * f32(i) / kRate);
        }
    }
}

}  // namespace

void make_clips(Clips& out) {
    sweep(out.jump, 0.16f, 260.0f, 720.0f, 0.22f, 0.0f, 10.0f);
    sweep(out.hurt, 0.40f, 420.0f, 90.0f, 0.30f, 0.45f, 5.0f);
    sweep(out.stomp, 0.14f, 180.0f, 55.0f, 0.45f, 0.30f, 14.0f);
    const i32 chime[] = {83, 88};
    notes(out.pickup, chime, 2, 0.07f, 0.18f);
    const i32 fanfare[] = {72, 76, 79, 84, 79, 84};
    notes(out.win, fanfare, 6, 0.11f, 0.20f);

    // Music: 120 bpm, 4 bars of C - Am - F - G; triangle bass on the beat, square arpeggio on
    // eighths. 8 s exactly, every note decays to zero inside its slot: the loop is seamless.
    constexpr f32 kBeat = 0.5f;
    constexpr i32 kChords[4][3] = {{60, 64, 67}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}};
    const usize beat = frames(kBeat), eighth = beat / 2;
    out.music.clear();
    out.music.resize(beat * 16);
    for (usize i = 0; i < out.music.size(); ++i) out.music[i] = 0.0f;
    for (usize b = 0; b < 16; ++b) {
        const i32* chord = kChords[b / 4];
        const f32 bass = midi_hz(chord[0] - 24 + (b % 2 == 1 ? 7 : 0));
        for (usize i = 0; i < beat; ++i)
            out.music[b * beat + i] += 0.30f * env(i, beat, 3.0f) * triangle(bass * f32(i) / kRate);
        for (usize e = 0; e < 2; ++e) {
            const usize step = b * 2 + e;
            const i32 note = chord[step % 3] + 12 + (step % 6 == 5 ? 12 : 0);
            const f32 hz = midi_hz(note);
            for (usize i = 0; i < eighth; ++i)
                out.music[b * beat + e * eighth + i] += 0.07f * env(i, eighth, 8.0f) * square(hz * f32(i) / kRate);
        }
    }
}

}  // namespace platformer::art
