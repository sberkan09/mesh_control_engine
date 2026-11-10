#pragma once
// Tracer lines for shots (M-TRACERS). kit::install_stock installs it.
//
//   Tracer3D  on an entity that also has a LocalTransform and a MeshRef whose mesh is a unit box centred on its origin
//   (an
//             emissive material makes it glow): one slot of a ring of tracers. Author as many as you want (16 is a good
//             number); they all start hidden.
//
// Every WeaponFired3D whose `origin` to `end` is at least `min_length` long takes the next slot: the one with the
// lowest `serial` (a never-used one first, then the oldest). The slot moves to the middle of the shot, turns its +Z
// along it, is scaled to (`width`, `width`, the shot's length), shown, and stamped `serial` as the highest serial plus
// one. After `life` seconds it is hidden again. Two shots in one tick take two slots; a shorter shot draws nothing.
//
// System: "kit_tracers" (FixedUpdate_Post). Needs install_combat3d first (it registers WeaponFired3D), as install_stock
// does.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct Tracer3D {
    f32 width = 0.04f;      // metres, the box's x and y
    f32 life = 0.06f;       // s a tracer stays visible
    f32 min_length = 1.0f;  // m: a shorter shot draws nothing
    f32 age = -1.0f;        // engine-written: s since it was placed; < 0 = unused (hidden)
    u32 serial = 0;         // engine-written: placement order, 0 = never used
};
MCE_COMPONENT(Tracer3D, (width, Range{0.001f, 2}), (life, Range{0, 10}, Help{"s visible"}),
              (min_length, Range{0, 1000}, Help{"m: shorter shots draw nothing"}), (age, ReadOnly), (serial, ReadOnly))

// Registers the component and adds the system. Idempotent.
void install_tracers(World& world);

}  // namespace mce::kit
