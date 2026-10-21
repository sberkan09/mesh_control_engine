#pragma once
// @PROJECT@ components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// (@GAME@_cook links types.cpp) validates them, and the systems in @GAME@.cpp run on them. Keep
// this header data-only (core + refl), so the cook can include it without the game's code.

#include "mce/refl/macros.h"
#include "mce/types.h"

namespace @GAME@ {

using mce::f32;

// Turns the entity about +Z (degrees per second).
struct Spin {
    f32 degrees_per_second = 90.0f;
};
MCE_COMPONENT(Spin, degrees_per_second)

// Moves the entity up and down around where the scene placed it.
struct Bob {
    f32 height = 0.5f;  // meters
    f32 period = 2.0f;  // seconds
    f32 base_y = 0.0f;  // set on the first tick
    f32 time = 0.0f;
    bool started = false;
};
MCE_COMPONENT(Bob, height, period, base_y, time, started)

}  // namespace @GAME@
