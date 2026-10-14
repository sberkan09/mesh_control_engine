#pragma once
// Sprite-sheet animation (M-DATA-REACH 5): the system that plays AnimatedSprite (components.h)
// clips from cooked .mcsheet sprite sheets onto the Sprite next to it, and the event its clips'
// frame events raise. kit::install_stock installs it.
//
//   world.events().read<kit::SpriteAnimEvent>()   // in FixedUpdate_Pre of the next tick, or later
//
// "kit_sprite_animation" runs in FixedUpdate_Post: it advances each playing clip by dt x speed on
// the fixed tick (render/sprite_animation.h: frames step, never blend), writes the sheet's texture
// and the frame's region into the Sprite, and pushes a SpriteAnimEvent for every frame event the
// playhead enters (frame 0's when a clip starts too).

#include "mce/ecs/entity.h"
#include "mce/types.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct SpriteAnimEvent {
    Entity entity;
    u32 id = 0;      // low 32 bits of fnv1a64(event name), as the sheet stores it
    u16 frame = 0;   // frame index within the clip
    u16 pad_ = 0;
};

// Registers AnimatedSprite and SpriteAnimEvent and adds "kit_sprite_animation". Idempotent; `assets`
// (may be null: nothing plays) loads the sheets.
void install_sprite_animation(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
