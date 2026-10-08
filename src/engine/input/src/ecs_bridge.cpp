#include "mce/input/ecs.h"

namespace mce::input {

void install(World& world) {
    if (!world.try_resource<InputState>()) world.set_resource(InputState{});
}

void publish(World& world, const InputState& state) { world.set_resource(state); }

}  // namespace mce::input
