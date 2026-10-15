#pragma once
// MarbleGarden gameplay: the install function the module entry hands the host.

#include "components.h"
#include "mce/gameplay.h"

namespace marble_garden {

// Events (produced in FixedUpdate_Post).
struct GemCollected {  // the marble picked up a gem
    mce::Entity gem;
    mce::u32 collected = 0;  // gems collected so far this round
    mce::u32 total = 0;
};
struct MarbleRespawned {  // the pond (or a fall) put the marble back at its respawn point
    mce::Entity marble;
    mce::Entity hazard;  // null for a fall off the world
};
struct RoundOver {  // the round ended
    mce::u32 won = 0;
    mce::u32 gems = 0;
    mce::u32 total = 0;
    mce::f32 time_used = 0;
};

// The last round's result, kept across scene changes (the win and lose screens read it).
struct GardenResult {
    mce::u32 rounds = 0;
    mce::u32 won = 0;
    mce::u32 gems = 0;
    mce::u32 total = 0;
    mce::u32 falls = 0;
    mce::f32 time_used = 0;
    mce::f32 best_time = 0;
};

// Registers the game's components and systems on top of the engine's.
mce::ResultV install(mce::World& world);

}  // namespace marble_garden
