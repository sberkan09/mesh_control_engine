#pragma once
// ui_screens sample components: plain reflected data the scene authors and the HUD binds
// ("bind": "Score/points" in ui/hud.mcui). Data only (core + refl), so mce_cook --module reads them.

#include "mce/refl/macros.h"
#include "mce/types.h"

namespace ui_screens {

using mce::f32;
using mce::i32;

// The run's score: a point per second of play, shown by the HUD document.
struct Score {
    i32 points = 0;
    f32 time = 0.0f;  // seconds played (the pause menu stops it)
};
MCE_COMPONENT(Score, points, time)

// Marks the UiScreen the game opens on Pause and closes on Resume / Cancel.
struct PauseMenu {
    i32 opened = 0;  // how many times it opened (the headless test reads it in the hash)
};
MCE_COMPONENT(PauseMenu, opened)

}  // namespace ui_screens
