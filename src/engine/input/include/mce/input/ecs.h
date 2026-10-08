#pragma once
// InputState as a World resource (audio-input-ui §2.1): the run loop samples a tick, publishes
// the snapshot, then ticks the world; FixedUpdate_Pre systems read f.resource<InputState>().

#include "mce/ecs/world.h"
#include "mce/input/actions.h"

namespace mce::input {

// Registers a zeroed InputState resource.
void install(World& world);
// Copies this tick's snapshot into the world's InputState resource.
void publish(World& world, const InputState& state);

}  // namespace mce::input
