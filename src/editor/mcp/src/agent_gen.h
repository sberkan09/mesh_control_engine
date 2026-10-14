#pragma once
// Procedural content generators for the M-AGENT content tools (ROADMAP M-AGENT step 5): palettes,
// pixel sprites, tile sets, sfxr-style sound effects, noise and sky textures, music loops. Pure
// functions of their parameters and seed: the same call gives the same output on one build (an
// integer RNG; float math may round differently between compilers). Private to mce_mcp_core.

#include "mce/containers/array.h"
#include "mce/cook/image.h"
#include "mce/cook/string.h"

namespace mce::mcp::gen {

// splitmix64: a small deterministic RNG.
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    u64 next();
    f32 unit();                      // [0, 1)
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
    i32 range_i(i32 lo, i32 hi);     // [lo, hi]
};

// ---------------------------------------------------------------- color

struct Rgb {
    f32 r = 0, g = 0, b = 0;
};
Rgb hsv(f32 h, f32 s, f32 v);  // h in turns (0..1)
u32 pack(Rgb c, u8 a = 255);   // 0xRRGGBBAA
Rgb unpack(u32 rgba);
Rgb mix(Rgb a, Rgb b, f32 t);
bool parse_hex(StringView s, u32& rgba);  // "#rrggbb" or "#rrggbbaa"
void to_hex(u32 rgba, cook::String& out);

// Palette schemes: analogous, complementary, triad, pastel, earth, neon, mono. Colors run dark to light.
bool palette_scheme_known(StringView scheme);
Array<u32> palette(u64 seed, u32 count, StringView scheme);

// ---------------------------------------------------------------- pixel art

// Sprite kinds: creature, character, ship, item, blob. Mirrored left-right, outlined, shaded with
// `colors` (dark to light; at least 3; the darkest outlines). Transparent background.
bool sprite_kind_known(StringView kind);
cook::Image pixel_sprite(u64 seed, StringView kind, u32 width, u32 height, Span<const u32> colors);

// Tile set styles: grass, stone, brick, dirt, sand, snow, water, wood, metal. One row of tiles,
// `tile` pixels square: fill, fill_2, fill_3, top, top_left, top_right, platform, decoration.
bool tile_style_known(StringView style);
inline constexpr const char* kTileNames[] = {"fill", "fill_2", "fill_3", "top", "top_left", "top_right", "platform", "decoration"};
inline constexpr u32 kTileCount = 8;
// `colors` empty: the style's own.
cook::Image tileset(u64 seed, StringView style, u32 tile, Span<const u32> colors);

// ---------------------------------------------------------------- textures

// Kinds: value, perlin, fbm, ridged, worley, sky (an equirect sky: gradient, clouds, `stars`).
// `colors` are gradient stops (low to high; sky: horizon, zenith, clouds); empty: greys. `tileable`
// wraps the noise (not for sky).
struct NoiseDesc {
    cook::String kind{"fbm"};
    u32 width = 256, height = 256;
    f32 scale = 4.0f;  // features across the image
    u32 octaves = 5;
    bool tileable = true;
    f32 stars = 0.0f;  // sky: 0..1 density
    f32 clouds = 0.5f; // sky: 0..1 cover
    Array<u32> colors;
};
bool noise_kind_known(StringView kind);
cook::Image noise_texture(u64 seed, const NoiseDesc& d);

// ---------------------------------------------------------------- audio

inline constexpr u32 kSampleRate = 44100;

// sfxr's parameters (DrPetter's sfxr, 2007; the same names and ranges, 0..1 or -1..1).
struct SfxParams {
    i32 wave = 0;  // 0 square, 1 saw, 2 sine, 3 noise
    f32 base_freq = 0.3f, freq_limit = 0.0f, freq_ramp = 0.0f, freq_dramp = 0.0f;
    f32 duty = 0.0f, duty_ramp = 0.0f;
    f32 vib_strength = 0.0f, vib_speed = 0.0f;
    f32 env_attack = 0.0f, env_sustain = 0.3f, env_punch = 0.0f, env_decay = 0.4f;
    f32 lpf_freq = 1.0f, lpf_ramp = 0.0f, lpf_resonance = 0.0f;
    f32 hpf_freq = 0.0f, hpf_ramp = 0.0f;
    f32 pha_offset = 0.0f, pha_ramp = 0.0f;
    f32 repeat_speed = 0.0f;
    f32 arp_speed = 0.0f, arp_mod = 0.0f;
    f32 volume = 0.5f;
};
// Presets: pickup, laser, explosion, powerup, hit, jump, blip, random.
bool sfx_preset_known(StringView preset);
SfxParams sfx_preset(u64 seed, StringView preset);
// Mono 16-bit samples at kSampleRate (at most a few seconds).
Array<i16> synth_sfx(const SfxParams& p, u64 seed);

// A seamless music loop: bass, chords, a melody and drums in `scale` (major, minor, pentatonic,
// blues, dorian) on `root` (0 = C .. 11 = B). Exactly `bars` bars of 4 beats at `bpm`.
struct MusicDesc {
    u32 bpm = 120;
    u32 bars = 8;
    u32 root = 0;
    cook::String scale{"major"};
    bool drums = true;
    f32 volume = 0.6f;
};
bool music_scale_known(StringView scale);
Array<i16> music_loop(u64 seed, const MusicDesc& d);

// 16-bit mono PCM WAV bytes.
void wav_bytes(Span<const i16> samples, u32 rate, Array<byte>& out);

}  // namespace mce::mcp::gen
