#pragma once
// Generated content for 03_platformer: a 16×16 pixel-art array atlas and synthesized sound effects
// plus a looping music track. The cook pipeline is not wired into samples yet, so everything is
// produced at startup, deterministically (no randomness beyond a fixed-seed LCG).

#include "mce/core.h"

namespace platformer::art {

using mce::Array;
using mce::f32;
using mce::u32;
using mce::u8;

inline constexpr u32 kTexels = 16;  // atlas layer edge; 16 texels = 1 m

// Atlas layers. Tiles are drawn through a second sprite set whose regions are the tile layers.
enum Layer : u32 {
    PlayerIdle,
    PlayerRunA,
    PlayerRunB,
    PlayerJump,
    EnemyCalm,
    EnemyAngry,
    Coin0,
    Coin1,
    Coin2,
    Coin3,
    Spike,
    Flag,
    Platform,
    GroundTop,
    GroundFill,
    Plank,
    White,
    Cloud,
    Hill,
    Crate,
    Crumble,     // cracked ember-stone block (X tiles)
    Dot,         // soft round particle
    PlayerDash,  // stretched pose
    LayerCount
};

// RGBA8 (sRGB) texels, LayerCount layers of kTexels × kTexels, row 0 = top.
void make_atlas(Array<u8>& out);

// 48 kHz mono float32 clips.
struct Clips {
    Array<f32> jump, pickup, hurt, stomp, win;
    Array<f32> music;  // loops seamlessly end -> start
};
void make_clips(Clips& out);

}  // namespace platformer::art
