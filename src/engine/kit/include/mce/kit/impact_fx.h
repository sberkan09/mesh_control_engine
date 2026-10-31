#pragma once
// Impact sparks where a shot lands (M-IMPACTFX). kit::install_stock installs it.
//
//   ImpactFx3D  on an entity with a ParticleEmitter3D (rate 0, a short burst look): every WeaponHit3D moves the
//               entity to the hit point (lifted `lift` m along the surface normal) and fires one burst of `count`
//               particles there on the next tick (the move has to reach the world transform first). One emitter serves all shooters; two hits in one tick show only the last, so the
//               effect is a pool of one. `fired` counts the bursts (engine-written).
//
// System: "kit_impact_fx" (FixedUpdate_Post, after "kit_effects").

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct ImpactFx3D {
    u32 count = 10;      // particles per burst
    f32 lift = 0.03f;    // metres along the surface normal, so the sparks do not sit inside the wall
    u32 fired = 0;       // engine-written
    u32 pending = 0;     // engine-written: a burst waits one tick, so the move has reached the world transform
};
MCE_COMPONENT(ImpactFx3D, (count, Range{1, 512}, Help{"particles per burst"}),
              (lift, Range{0, 1}, Help{"metres off the surface along its normal"}), (fired, ReadOnly, Transient), (pending, ReadOnly, Transient))

// Registers the component and adds the system. Idempotent.
void install_impact_fx(World& world);

}  // namespace mce::kit
