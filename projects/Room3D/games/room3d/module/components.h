#pragma once
// Room3D components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// (room3d_cook links types.cpp) validates them, and the systems in room3d.cpp run on them. Keep
// this header data-only (core + refl + the kit's 3D data), so the cook can include it without the
// game's code. Layouts stay padding-free: the determinism hash reads component bytes.

#include "mce/kit/kit3d.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace room3d {

using mce::f32;
using mce::u32;

// The walking hero: a capsule character in the kit's Physics3D world, driven by Move/Jump relative
// to the follow camera. Its MeshRef + Animator switch between "idle" and "walk".
struct Hero {
    f32 walk_speed = 4.5f;  // m/s
    f32 jump_speed = 6.0f;  // m/s
    f32 yaw = 0.0f;         // radians, facing (mesh +Z forward)
    f32 vy = 0.0f;          // vertical speed
    f32 speed = 0.0f;       // achieved horizontal speed last tick
    u32 grounded = 0;
    u32 created = 0;        // the character was made once (Physics3D itself is not saved)
    u32 jumps = 0;          // jumps taken (tests read it)
};
MCE_COMPONENT(Hero, walk_speed, jump_speed, yaw, vy, speed, grounded, created, jumps)

// A third-person boom behind the first Hero: Look turns it, the camera entity's LocalTransform
// follows. The kit's extract interpolates it between ticks.
struct FollowCamera {
    f32 distance = 5.0f;  // metres behind the pivot
    f32 height = 1.6f;    // pivot height above the hero's feet
    f32 yaw = 0.0f;       // radians about +Y; 0 looks down -Z
    f32 pitch = 0.3f;     // radians, positive looks down
    f32 sensitivity = 0.0045f;
    u32 pad_ = 0;
};
MCE_COMPONENT(FollowCamera, distance, height, yaw, pitch, sensitivity, pad_)

// Scatters `count` rocks in a ring around the entity at Startup into the kit's StaticInstances3D
// (one draw list culled per cell, thinned with distance). Deterministic from `seed`.
struct RockField {
    mce::asset::AssetRef<mce::asset::Mesh> rock;
    u32 count = 500;
    f32 inner = 16.0f;  // metres
    f32 outer = 140.0f;
    u32 seed = 1;
};
MCE_COMPONENT(RockField, rock, count, inner, outer, seed)

}  // namespace room3d
